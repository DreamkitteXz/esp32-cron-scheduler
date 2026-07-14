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
    TEST_IGNORE_MESSAGE("TODO(dev): cron_parse - campos '*'");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("* * * * * *", &e));
    TEST_ASSERT_EQUAL_HEX64(0x0FFFFFFFFFFFFFFFULL, e.sec); /* bits 0..59 */
    TEST_ASSERT_EQUAL_HEX32(0x00FFFFFFU, e.hour);          /* bits 0..23 */
    TEST_ASSERT_EQUAL_HEX32(0x0000007FU, e.wday);          /* bits 0..6 */
}

static void test_parse_valor_unico(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_parse - valor único");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 30 8 * * *", &e));
    TEST_ASSERT_EQUAL_HEX64(1ULL << 0, e.sec);
    TEST_ASSERT_EQUAL_HEX64(1ULL << 30, e.min);
    TEST_ASSERT_EQUAL_HEX32(1U << 8, e.hour);
}

static void test_parse_lista_intervalo_e_passo(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_parse - listas, intervalos e passos");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 8,12,18 * * *", &e)); /* lista */
    TEST_ASSERT_EQUAL_HEX32((1U << 8) | (1U << 12) | (1U << 18), e.hour);

    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 0 * * * 1-5", &e)); /* intervalo: seg a sex */
    TEST_ASSERT_EQUAL_HEX32(0x3EU, e.wday);

    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("*/15 * * * * *", &e)); /* passo */
    TEST_ASSERT_EQUAL_HEX64((1ULL << 0) | (1ULL << 15) | (1ULL << 30) | (1ULL << 45), e.sec);
}

static void test_parse_numero_de_campos_errado(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_parse - contagem de campos");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_FIELDS, cron_parse("* * * * *", &e));     /* 5 campos */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_FIELDS, cron_parse("* * * * * * *", &e)); /* 7 campos */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_FIELDS, cron_parse("", &e));
}

static void test_parse_fora_do_range(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_parse - validação de range");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("60 * * * * *", &e)); /* segundo > 59 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * 24 * * *", &e)); /* hora > 23 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * 0 * *", &e));  /* dia < 1 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * * 13 *", &e)); /* mês > 12 */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_RANGE, cron_parse("* * * * * 7", &e));  /* dia-da-semana > 6 */
}

static void test_parse_sintaxe_invalida(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_parse - sintaxe");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("a * * * * *", &e));
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX, cron_parse("*/0 * * * * *", &e)); /* passo zero */
    TEST_ASSERT_EQUAL_INT(CRON_ERR_SYNTAX,
                          cron_parse("5-1 * * * * *", &e)); /* intervalo ao contrário */
}

static void test_match_casa_e_nao_casa(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_match");

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
    TEST_IGNORE_MESSAGE("TODO(dev): cron_match - regra OR entre mday e wday");

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

static void test_next_run(void) {
    TEST_IGNORE_MESSAGE("TODO(dev): cron_next_run");

    cron_expr_t e;
    TEST_ASSERT_EQUAL_INT(CRON_OK, cron_parse("0 30 8 * * *", &e));

    struct tm base = mk_tm(2026, 7, 14, 8, 29, 59);
    const time_t next = cron_next_run(&e, mktime(&base));

    struct tm want = mk_tm(2026, 7, 14, 8, 30, 0);
    TEST_ASSERT_EQUAL_INT64((int64_t)mktime(&want), (int64_t)next);
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
    RUN_TEST(test_match_casa_e_nao_casa);
    RUN_TEST(test_match_dia_do_mes_ou_dia_da_semana);
    RUN_TEST(test_next_run);

    return UNITY_END();
}
