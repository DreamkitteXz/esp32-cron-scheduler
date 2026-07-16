#include "cron.h"

#include <string.h>

/*
 * Premissa registrada: a tabela de ranges do enunciado veio corrompida ("0-5U"). Assumimos:
 *   segundo 0-59 | minuto 0-59 | hora 0-23 | dia 1-31 | mês 1-12 | dia-da-semana 0-6 (0=domingo)
 * Ver README, seção "Decisões de projeto".
 *
 * Este componente é C puro de propósito: nenhum header do ESP-IDF, do FreeRTOS ou de driver.
 * É isso que permite compilá-lo no host e testá-lo com Unity/CTest sem hardware.
 *
 * Cada campo vira um bitmask: parse uma vez, e o match em runtime é um teste de bit por campo.
 * Toda a gramática de um item — "*", "N", "N-M", "*\/S", "N-M/S" — colapsa numa única operação:
 * acender os bits de [low, high] com passo step (set_range).
 */

/* Range fechado [min, max] de cada campo, na ordem canônica do CRON. */
static const struct {
    int min;
    int max;
} kFieldRange[6] = {
    {0, 59}, /* segundo        */
    {0, 59}, /* minuto         */
    {0, 23}, /* hora           */
    {1, 31}, /* dia do mês     */
    {1, 12}, /* mês            */
    {0, 6},  /* dia da semana  */
};

/* Acende os bits low, low+step, ..., <= high. É aqui que lista/intervalo/passo convergem. */
static void set_range(uint64_t *mask, int low, int high, int step) {
    for (int v = low; v <= high; v += step) {
        *mask |= (uint64_t)1 << v;
    }
}

/*
 * Converte [s, e) num inteiro não-negativo, aceitando SÓ dígitos.
 * Devolve false para vazio ("") ou qualquer caractere não-numérico ("1a", "-5", " 5").
 * É essa rigidez que transforma tokens malformados em erro em vez de valores silenciosos.
 */
static bool str2int(const char *s, const char *e, int *out) {
    if (s >= e) {
        return false;
    }
    long v = 0;
    for (const char *p = s; p < e; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + (*p - '0');
        if (v > 1000000) { /* guarda de overflow; muito acima de qualquer max de campo */
            return false;
        }
    }
    *out = (int)v;
    return true;
}

/*
 * Parseia um item (sem vírgulas) da gramática: "*" | "N" | "N-M" | "*\/S" | "N-M/S".
 * Um passo (/S) só é válido acompanhando "*" ou um intervalo "N-M" — um "N/S" isolado não faz
 * parte da gramática do enunciado e é rejeitado como sintaxe.
 */
static cron_err_t parse_item(const char *s, const char *e, int fmin, int fmax, uint64_t *mask) {
    /* Separa o passo opcional depois de '/'. */
    int step = 1;
    const char *slash = memchr(s, '/', (size_t)(e - s));
    const char *range_end = e;
    if (slash != NULL) {
        range_end = slash;
        int st;
        if (!str2int(slash + 1, e, &st) || st == 0) {
            return CRON_ERR_SYNTAX; /* passo ausente, zero ou nao-numerico */
        }
        step = st;
    }

    /* Curinga: todo o range do campo. */
    if (range_end - s == 1 && s[0] == '*') {
        set_range(mask, fmin, fmax, step);
        return CRON_OK;
    }

    /* Intervalo "N-M". */
    const char *dash = memchr(s, '-', (size_t)(range_end - s));
    if (dash != NULL) {
        int low, high;
        if (!str2int(s, dash, &low) || !str2int(dash + 1, range_end, &high)) {
            return CRON_ERR_SYNTAX; /* "-5", "5-", "a-5", "5-b" */
        }
        if (low < fmin || high > fmax) {
            return CRON_ERR_RANGE;
        }
        if (low > high) {
            return CRON_ERR_RANGE; /* "5-1": sem wrap-around, é erro de range */
        }
        set_range(mask, low, high, step);
        return CRON_OK;
    }

    /* Valor único "N" — nunca com passo (grammar não prevê "N/S"). */
    if (slash != NULL) {
        return CRON_ERR_SYNTAX;
    }
    int v;
    if (!str2int(s, range_end, &v)) {
        return CRON_ERR_SYNTAX; /* "1a", "" */
    }
    if (v < fmin || v > fmax) {
        return CRON_ERR_RANGE;
    }
    set_range(mask, v, v, 1);
    return CRON_OK;
}

/*
 * Parseia um campo inteiro (lista de itens separados por ','), acendendo os bits em *mask.
 * Um item vazio — de ",5", "5,", "1,,3" — é erro: strtok esconderia isso, então varremos à mão.
 */
static cron_err_t parse_field(const char *s, const char *e, int fmin, int fmax, uint64_t *mask) {
    *mask = 0;
    const char *item = s;
    for (;;) {
        const char *comma = memchr(item, ',', (size_t)(e - item));
        const char *item_end = (comma != NULL) ? comma : e;
        if (item_end == item) {
            return CRON_ERR_SYNTAX; /* item vazio */
        }
        cron_err_t err = parse_item(item, item_end, fmin, fmax, mask);
        if (err != CRON_OK) {
            return err;
        }
        if (comma == NULL) {
            return CRON_OK;
        }
        item = comma + 1;
    }
}

/* "*" literal — note que "*\/2" NÃO é star: tem passo, logo é restrito. */
static bool field_is_star(const char *s, const char *e) { return (e - s == 1) && (s[0] == '*'); }

cron_err_t cron_parse(const char *expr, cron_expr_t *out) {
    if (expr == NULL || out == NULL) {
        return CRON_ERR_SYNTAX;
    }

    /* Delimita os campos por espaços em branco, sem mutar `expr`. */
    const char *field_s[6];
    const char *field_e[6];
    int nfields = 0;
    for (const char *p = expr; *p != '\0';) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (nfields < 6) {
            field_s[nfields] = start;
            field_e[nfields] = p;
        }
        nfields++;
        if (nfields > 6) {
            return CRON_ERR_FIELDS; /* campos demais */
        }
    }
    if (nfields != 6) {
        return CRON_ERR_FIELDS; /* campos de menos (inclui expressão vazia) */
    }

    /* Parseia num temporário: *out só é tocado se os 6 campos forem válidos. */
    uint64_t m[6];
    for (int i = 0; i < 6; i++) {
        cron_err_t err =
            parse_field(field_s[i], field_e[i], kFieldRange[i].min, kFieldRange[i].max, &m[i]);
        if (err != CRON_OK) {
            return err;
        }
    }

    out->sec = m[0];
    out->min = m[1];
    out->hour = (uint32_t)m[2];
    out->mday = (uint32_t)m[3];
    out->mon = (uint32_t)m[4];
    out->wday = (uint32_t)m[5];

    /* Star-ness é sintática (ver cron.h): só o campo escrito exatamente "*" conta. */
    out->mday_star = field_is_star(field_s[3], field_e[3]);
    out->wday_star = field_is_star(field_s[5], field_e[5]);
    return CRON_OK;
}

/*
 * Testes de bit com verificação de faixa. O `struct tm` vem de fora (e tm_sec pode legitimamente
 * ser 60 num leap second), então um valor fora da faixa não pode virar shift além da largura do
 * tipo — isso seria comportamento indefinido, não só um resultado errado. Fora da faixa = não casa.
 */
static bool bit_test64(uint64_t mask, int v) {
    if (v < 0 || v > 63) {
        return false;
    }
    return ((mask >> v) & 1u) != 0;
}

static bool bit_test32(uint32_t mask, int v) {
    if (v < 0 || v > 31) {
        return false;
    }
    return ((mask >> v) & 1u) != 0;
}

/*
 * Regra do dia, isolada porque cron_match e cron_next_run precisam concordar exatamente:
 * ambos restritos => OR ("dia 1 OU toda segunda"). Se pelo menos um é "*", o AND é o certo — e
 * o campo "*" casa sempre, então o AND se reduz ao outro campo.
 */
static bool day_matches(const cron_expr_t *e, const struct tm *t) {
    const bool mday_hit = bit_test32(e->mday, t->tm_mday);
    const bool wday_hit = bit_test32(e->wday, t->tm_wday);

    if (e->mday_star || e->wday_star) {
        return mday_hit && wday_hit;
    }
    return mday_hit || wday_hit;
}

bool cron_match(const cron_expr_t *e, const struct tm *t) {
    if (e == NULL || t == NULL) {
        return false;
    }

    /* Campos independentes: AND direto. tm_mon é 0-11, a máscara é 1-12. */
    return bit_test64(e->sec, t->tm_sec) && bit_test64(e->min, t->tm_min) &&
           bit_test32(e->hour, t->tm_hour) && bit_test32(e->mon, t->tm_mon + 1) &&
           day_matches(e, t);
}

/* newlib (ESP-IDF) oferece localtime_r; a UCRT do MinGW só tem localtime_s, com os args trocados.
 */
static bool localtime_safe(const time_t *t, struct tm *out) {
#if defined(_WIN32)
    return localtime_s(out, t) == 0;
#else
    return localtime_r(t, out) != NULL;
#endif
}

/* Teto da busca. 4 anos cobrem o ciclo bissexto, então "30 de fevereiro" termina em vez de travar.
 */
#define CRON_SEARCH_MAX_YEARS 4

time_t cron_next_run(const cron_expr_t *e, time_t after) {
    if (e == NULL) {
        return (time_t)-1;
    }

    time_t candidate = after + 1; /* estritamente depois de `after` */
    struct tm t;
    if (!localtime_safe(&candidate, &t)) {
        return (time_t)-1;
    }
    const int year_limit = t.tm_year + CRON_SEARCH_MAX_YEARS;

    /*
     * Busca por salto de campo, do mais grosso para o mais fino: se o mês não casa, pula para o
     * mês seguinte inteiro em vez de testar cada segundo dele. Uma varredura segundo a segundo
     * levaria ~126 milhões de iterações para provar que "30/02" nunca ocorre; assim são poucas
     * dezenas. Cada salto zera os campos abaixo e passa por mktime, que normaliza os estouros
     * (dia 32 vira dia 1 do mês seguinte) e recalcula tm_wday.
     */
    for (;;) {
        if (t.tm_year > year_limit) {
            return (time_t)-1; /* expressão que nunca ocorre */
        }

        if (!bit_test32(e->mon, t.tm_mon + 1)) {
            t.tm_mon++;
            t.tm_mday = 1;
            t.tm_hour = t.tm_min = t.tm_sec = 0;
        } else if (!day_matches(e, &t)) {
            t.tm_mday++;
            t.tm_hour = t.tm_min = t.tm_sec = 0;
        } else if (!bit_test32(e->hour, t.tm_hour)) {
            t.tm_hour++;
            t.tm_min = t.tm_sec = 0;
        } else if (!bit_test64(e->min, t.tm_min)) {
            t.tm_min++;
            t.tm_sec = 0;
        } else if (!bit_test64(e->sec, t.tm_sec)) {
            t.tm_sec++;
        } else {
            t.tm_isdst = -1; /* deixa a libc resolver o fuso do instante encontrado */
            return mktime(&t);
        }

        /* Normaliza o salto (e recalcula tm_wday) antes da próxima volta. */
        t.tm_isdst = -1;
        const time_t norm = mktime(&t);
        if (norm == (time_t)-1) {
            return (time_t)-1;
        }
        if (!localtime_safe(&norm, &t)) {
            return (time_t)-1;
        }
    }
}

const char *cron_strerror(cron_err_t err) {
    switch (err) {
        case CRON_OK:
            return "ok";
        case CRON_ERR_FIELDS:
            return "expressao deve ter 6 campos: seg min hora dia mes dia-da-semana";
        case CRON_ERR_RANGE:
            return "valor fora do intervalo permitido para o campo";
        case CRON_ERR_SYNTAX:
            return "sintaxe invalida";
        default:
            return "erro desconhecido";
    }
}
