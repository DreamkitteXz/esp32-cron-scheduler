#ifndef CRON_H_
#define CRON_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Expressão CRON de 6 campos, pré-compilada em bitmasks.
 *
 * Cada campo guarda um bit por valor possível. Isso torna o match O(1) (um teste de bit por
 * campo) e faz listas ("1,5"), intervalos ("1-5") e passos ("*\/15") caírem de graça: todos
 * viram simplesmente "acender um conjunto de bits" no parse.
 */
typedef struct {
    uint64_t sec;  /**< bits 0..59 */
    uint64_t min;  /**< bits 0..59 */
    uint32_t hour; /**< bits 0..23 */
    uint32_t mday; /**< bits 1..31 */
    uint32_t mon;  /**< bits 1..12 */
    uint32_t wday; /**< bits 0..6, 0 = domingo */
} cron_expr_t;

typedef enum {
    CRON_OK = 0,     /**< expressão válida */
    CRON_ERR_FIELDS, /**< número de campos != 6 */
    CRON_ERR_RANGE,  /**< valor fora do range do campo */
    CRON_ERR_SYNTAX, /**< caractere inesperado / campo malformado */
} cron_err_t;

/** Compila `expr` (6 campos separados por espaço) em bitmasks. Não aloca. */
cron_err_t cron_parse(const char *expr, cron_expr_t *out);

/** True se o instante `t` (hora local já resolvida) casa com a expressão. */
bool cron_match(const cron_expr_t *e, const struct tm *t);

/** Primeiro instante estritamente > `after` que casa com `e`, ou (time_t)-1 se não houver. */
time_t cron_next_run(const cron_expr_t *e, time_t after);

/** Mensagem estática para um código de erro. Nunca retorna NULL. */
const char *cron_strerror(cron_err_t err);

#ifdef __cplusplus
}
#endif

#endif  // CRON_H_
