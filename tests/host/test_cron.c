#include <string.h>
#include <time.h>

#include "cron.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Helper: monta um struct tm de hora local a partir dos campos. */
static struct tm mk_tm(int year, int mon, int mday, int hour, int min, int sec) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    mktime(&t); /* normaliza e preenche tm_wday */
    return t;
}

/* --- Testes que já valem hoje ---------------------------------------------------------------
 * cron_strerror() e as guardas de ponteiro nulo estão implementadas (não são lógica de negócio),
 * então estes rodam de verdade: o binário de teste não é uma casca vazia desde o primeiro commit.
 */

static void test_strerror_nunca_retorna_null(void) {
    TEST_ASSERT_EQUAL_STRING("ok", cron_strerror(CRON_OK));
    TEST_ASSERT_NOT_NULL(cron_strerror(CRON_ERR_FIELDS));
    TEST_ASSERT_NOT_NULL(cron_strerror(CRON_ERR_RANGE));
    TEST_ASSERT_NOT_NULL(cron_strerror(CRON_ERR_SYNTAX));
    TEST_ASSERT_NOT_NULL(cron_strerror((cron_err_t)999));
}

static void test_parse_rejeita_null(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse(NULL, &e));
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("* * * * * *", NULL));
}

static void test_match_rejeita_null(void) {
    const cron_expr_t e = {0};
    const struct tm t = mk_tm(2026, 7, 14, 12, 0, 0);
    TEST_ASSERT_FALSE(cron_match(NULL, &t));
    TEST_ASSERT_FALSE(cron_match(&e, NULL));
}

/* --- Testes da lógica a implementar -----------------------------------------------------------
 * Ficam IGNORE enquanto os TODO(dev) de cron.c não forem preenchidos: a suíte fica verde (a CI
 * valida build e plumbing) sem mentir que o parser funciona. Remova o TEST_IGNORE_MESSAGE de cada
 * um conforme implementar — é a lista de trabalho do componente, em ordem.
 */

static void test_parse_todos_curingas(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("* * * * * *", &e));
    TEST_ASSERT_EQUAL_HEX64(0x0FFFFFFFFFFFFFFFULL, e.sec); /* bits 0..59 */
    TEST_ASSERT_EQUAL_HEX32(0x00FFFFFFU, e.hour);          /* bits 0..23 */
    TEST_ASSERT_EQUAL_HEX32(0x0000007FU, e.wday);          /* bits 0..6 */
}

static void test_parse_valor_unico(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 30 8 * * *", &e));
    TEST_ASSERT_EQUAL_HEX64(1ULL << 0, e.sec);
    TEST_ASSERT_EQUAL_HEX64(1ULL << 30, e.min);
    TEST_ASSERT_EQUAL_HEX32(1U << 8, e.hour);
}

static void test_parse_lista_intervalo_e_passo(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 8,12,18 * * *", &e)); /* lista */
    TEST_ASSERT_EQUAL_HEX32((1U << 8) | (1U << 12) | (1U << 18), e.hour);

    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 * * * 1-5", &e)); /* intervalo: seg a sex */
    TEST_ASSERT_EQUAL_HEX32(0x3EU, e.wday);

    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("*/15 * * * * *", &e)); /* passo */
    TEST_ASSERT_EQUAL_HEX64((1ULL << 0) | (1ULL << 15) | (1ULL << 30) | (1ULL << 45), e.sec);
}

static void test_parse_numero_de_campos_errado(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_FIELDS, cron_parse("* * * * *", &e));     /* 5 campos */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_FIELDS, cron_parse("* * * * * * *", &e)); /* 7 campos */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_FIELDS, cron_parse("", &e));
}

static void test_parse_fora_do_range(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("60 * * * * *", &e)); /* segundo > 59 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * 24 * * *", &e)); /* hora > 23 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * 0 * *", &e));  /* dia < 1 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * * 13 *", &e)); /* mês > 12 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * * * 7", &e));  /* dia-da-semana > 6 */

    /* Premissa: intervalo invertido é erro de range, sem wrap-around. */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("5-1 * * * * *", &e)); /* low > high */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * * 12-1 *", &e));
}

static void test_parse_sintaxe_invalida(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("a * * * * *", &e));   /* nao-numerico */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("*/0 * * * * *", &e)); /* passo zero */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("1a * * * * *", &e));  /* lixo no fim */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("5/2 * * * * *", &e)); /* "N/S" nao existe */
}

/* Itens vazios e malformados: strtok os esconderia, então o parser precisa pegá-los à mão. */
static void test_parse_itens_vazios_e_malformados(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("1,,3 * * * * *", &e)); /* vírgula dupla */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse(",5 * * * * *", &e));   /* vírgula à frente */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("5, * * * * *", &e));   /* vírgula ao fim */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("1- * * * * *", &e));   /* fim do intervalo */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("-5 * * * * *", &e));   /* início ausente */
}

static void test_match_casa_e_nao_casa(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 30 8 * * *", &e)); /* 08:30:00 todo dia */

    const struct tm hit = mk_tm(2026, 7, 14, 8, 30, 0);
    const struct tm miss_min = mk_tm(2026, 7, 14, 8, 31, 0);
    const struct tm miss_sec = mk_tm(2026, 7, 14, 8, 30, 1);

    TEST_ASSERT_TRUE(cron_match(&e, &hit));
    TEST_ASSERT_FALSE(cron_match(&e, &miss_min));
    TEST_ASSERT_FALSE(cron_match(&e, &miss_sec));
}

static void test_match_dia_do_mes_ou_dia_da_semana(void) {
    cron_expr_t e;
    /* Ambos restritos: o cron clássico casa se QUALQUER um bater (OR), não os dois (AND). */
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 1 * 1", &e));

    const struct tm dia1 = mk_tm(2026, 7, 1, 0, 0, 0);    /* dia 1, quarta */
    const struct tm segunda = mk_tm(2026, 7, 6, 0, 0, 0); /* dia 6, segunda */
    const struct tm nenhum = mk_tm(2026, 7, 7, 0, 0, 0);  /* dia 7, terça */

    TEST_ASSERT_TRUE(cron_match(&e, &dia1));
    TEST_ASSERT_TRUE(cron_match(&e, &segunda));
    TEST_ASSERT_FALSE(cron_match(&e, &nenhum));
}

/* Contraponto do OR: com só UM dos dois campos restrito, a combinação volta a ser AND. */
static void test_match_um_campo_restrito_usa_and(void) {
    cron_expr_t e;

    /* Só dia-da-semana restrito: casa toda segunda, em qualquer dia do mês. */
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 * * 1", &e));
    const struct tm segunda = mk_tm(2026, 7, 6, 0, 0, 0);
    const struct tm terca = mk_tm(2026, 7, 7, 0, 0, 0);
    TEST_ASSERT_TRUE(cron_match(&e, &segunda));
    TEST_ASSERT_FALSE(cron_match(&e, &terca));

    /* Só dia-do-mês restrito: casa todo dia 6, seja qual for o dia da semana. */
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 6 * *", &e));
    TEST_ASSERT_TRUE(cron_match(&e, &segunda)); /* 6/jul é dia 6 */
    TEST_ASSERT_FALSE(cron_match(&e, &terca));  /* 7/jul não */
}

/*
 * Star-ness é sintática, não semântica: "1-31" acende os mesmos bits que "*", mas conta como
 * restrito — então com wday restrito a regra vira OR e a expressão casa todo dia.
 */
static void test_match_intervalo_cheio_conta_como_restrito(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 1-31 * 1", &e));

    const struct tm terca = mk_tm(2026, 7, 7, 0, 0, 0); /* não é segunda... */
    TEST_ASSERT_TRUE(cron_match(&e, &terca));           /* ...mas casa via OR do dia-do-mês */

    /* Com "*" no lugar de "1-31", vira AND: só segundas. */
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 * * 1", &e));
    TEST_ASSERT_FALSE(cron_match(&e, &terca));
}

/* tm_mon é 0-11 e a máscara é 1-12: um off-by-one aqui passaria despercebido sem teste. */
static void test_match_mes_off_by_one(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 1 1 *", &e)); /* 1º de janeiro */

    const struct tm jan1 = mk_tm(2026, 1, 1, 0, 0, 0);
    const struct tm fev1 = mk_tm(2026, 2, 1, 0, 0, 0);
    TEST_ASSERT_TRUE(cron_match(&e, &jan1));
    TEST_ASSERT_FALSE(cron_match(&e, &fev1));
}

/* Helper: cron_next_run a partir de um instante local, comparado com outro instante local. */
static void assert_next_run(const char *expr, struct tm from, struct tm want) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse(expr, &e));
    const time_t next = cron_next_run(&e, mktime(&from));
    TEST_ASSERT_EQUAL_INT64((int64_t)mktime(&want), (int64_t)next);
}

static void test_next_run(void) {
    /* Próximo segundo, no mesmo dia. */
    assert_next_run("0 30 8 * * *", mk_tm(2026, 7, 14, 8, 29, 59), mk_tm(2026, 7, 14, 8, 30, 0));
}

static void test_next_run_e_estritamente_depois(void) {
    /* Casando exatamente em `after`, deve pular para a próxima ocorrência, não devolver `after`. */
    assert_next_run("0 30 8 * * *", mk_tm(2026, 7, 14, 8, 30, 0), mk_tm(2026, 7, 15, 8, 30, 0));
}

static void test_next_run_vira_dia_mes_e_ano(void) {
    /* Vira o dia. */
    assert_next_run("0 0 8 * * *", mk_tm(2026, 7, 14, 9, 0, 0), mk_tm(2026, 7, 15, 8, 0, 0));
    /* Vira o mês. */
    assert_next_run("0 0 0 1 * *", mk_tm(2026, 7, 14, 0, 0, 0), mk_tm(2026, 8, 1, 0, 0, 0));
    /* Vira o ano. */
    assert_next_run("0 0 0 1 1 *", mk_tm(2026, 7, 14, 0, 0, 0), mk_tm(2027, 1, 1, 0, 0, 0));
}

static void test_next_run_29_de_fevereiro(void) {
    /* Existe, mas só em ano bissexto: exige atravessar ~2 anos de busca. */
    assert_next_run("0 0 0 29 2 *", mk_tm(2026, 7, 14, 0, 0, 0), mk_tm(2028, 2, 29, 0, 0, 0));
}

/*
 * Expressão impossível: 30/02 nunca ocorre. Prova que a busca termina (via teto de 4 anos) em vez
 * de girar para sempre — e, com o salto por campo, isso custa dezenas de iterações, não milhões.
 */
static void test_next_run_expressao_impossivel(void) {
    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 0 30 2 *", &e));

    struct tm base = mk_tm(2026, 7, 14, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64((int64_t)-1, (int64_t)cron_next_run(&e, mktime(&base)));
}

static void test_next_run_rejeita_null(void) {
    TEST_ASSERT_EQUAL_INT64((int64_t)-1, (int64_t)cron_next_run(NULL, 0));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_strerror_nunca_retorna_null);
    RUN_TEST(test_parse_rejeita_null);
    RUN_TEST(test_match_rejeita_null);

    RUN_TEST(test_parse_todos_curingas);
    RUN_TEST(test_parse_valor_unico);
    RUN_TEST(test_parse_lista_intervalo_e_passo);
    RUN_TEST(test_parse_numero_de_campos_errado);
    RUN_TEST(test_parse_fora_do_range);
    RUN_TEST(test_parse_sintaxe_invalida);
    RUN_TEST(test_parse_itens_vazios_e_malformados);
    RUN_TEST(test_match_casa_e_nao_casa);
    RUN_TEST(test_match_dia_do_mes_ou_dia_da_semana);
    RUN_TEST(test_match_um_campo_restrito_usa_and);
    RUN_TEST(test_match_intervalo_cheio_conta_como_restrito);
    RUN_TEST(test_match_mes_off_by_one);

    RUN_TEST(test_next_run);
    RUN_TEST(test_next_run_e_estritamente_depois);
    RUN_TEST(test_next_run_vira_dia_mes_e_ano);
    RUN_TEST(test_next_run_29_de_fevereiro);
    RUN_TEST(test_next_run_expressao_impossivel);
    RUN_TEST(test_next_run_rejeita_null);

    return UNITY_END();
}
