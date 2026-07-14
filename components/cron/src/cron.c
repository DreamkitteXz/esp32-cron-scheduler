#include "cron.h"

#include <string.h>

/*
 * Premissa registrada: a tabela de ranges do enunciado veio corrompida ("0-5U"). Assumimos:
 *   segundo 0-59 | minuto 0-59 | hora 0-23 | dia 1-31 | mês 1-12 | dia-da-semana 0-6 (0=domingo)
 * Ver README, seção "Decisões de projeto".
 *
 * Este componente é C puro de propósito: nada de esp_*, freertos/* ou driver/*. É isso que
 * permite compilá-lo no host e testá-lo com Unity/CTest sem hardware.
 */

cron_err_t cron_parse(const char *expr, cron_expr_t *out) {
    if (expr == NULL || out == NULL) {
        return CRON_ERR_SYNTAX;
    }
    memset(out, 0, sizeof(*out));

    // TODO(dev): quebrar `expr` em exatamente 6 campos separados por espaço (senão CRON_ERR_FIELDS)
    // e, para cada campo, acender os bits do range correspondente tratando "*", listas "a,b",
    // intervalos "a-b" e passos "*/n" / "a-b/n". Valor fora do range -> CRON_ERR_RANGE.
    return CRON_ERR_SYNTAX;
}

bool cron_match(const cron_expr_t *e, const struct tm *t) {
    if (e == NULL || t == NULL) {
        return false;
    }

    // TODO(dev): testar um bit por campo contra `t` e devolver o AND dos 6. Atenção à regra
    // clássica do cron: quando mday e wday são ambos restritos (nenhum é "*"), a combinação é OR.
    return false;
}

time_t cron_next_run(const cron_expr_t *e, time_t after) {
    if (e == NULL) {
        return (time_t)-1;
    }
    (void)after;

    // TODO(dev): varrer para frente a partir de `after`+1s até achar o primeiro instante que casa,
    // com teto (ex.: 4 anos, para cobrir 29/02) devolvendo (time_t)-1 se não houver. Saltar por
    // campo em vez de segundo a segundo mantém isso barato.
    return (time_t)-1;
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
