#ifndef SCHED_H_
#define SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "app_types.h"
#include "cron.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_MAX_ENTRIES 10 /* requisito: no máximo 10 agendamentos */
#define SCHED_EXPR_MAXLEN 64 /* expressão CRON crua, guardada para o comando `list` */

/** Uma linha da tabela de agendamentos. */
typedef struct {
    int id;                           /**< 0..SCHED_MAX_ENTRIES-1 */
    sched_action_t action;            /**< o que fazer quando casar */
    cron_expr_t expr;                 /**< expressão já compilada em bitmasks */
    char expr_str[SCHED_EXPR_MAXLEN]; /**< texto original, para exibir no `list` */
    time_t last_fire;                 /**< último disparo, 0 se nunca disparou */
} sched_entry_t;

/** Cria mutex, fila de comandos, task e o software timer de 1 Hz. */
esp_err_t sched_init(void);

/* As funções abaixo são chamadas pela CLI. Nenhuma delas toca a tabela diretamente: cada uma
 * enfileira um comando para a task do scheduler (a dona exclusiva da tabela) e espera a resposta
 * com timeout. Isso mantém a tabela com um único escritor e a CLI sem acesso ao mutex. */

/** Compila `cron_expr`, ocupa um slot livre e devolve o id em `out_id`. */
esp_err_t sched_add(const char *cron_expr, sched_action_t action, int *out_id);

/** Libera o slot `id`. ESP_ERR_NOT_FOUND se já estava livre. */
esp_err_t sched_del(int id);

/** Libera todos os slots. */
esp_err_t sched_clear(void);

/** Copia as entradas ativas para `buf` (copy-out sob mutex, sem expor ponteiro da tabela). */
esp_err_t sched_snapshot(sched_entry_t *buf, size_t cap, size_t *out_count);

/** Chamado pelo clk após `time set` ou sync NTP: rebaseline em vez de varrer o tempo perdido. */
void sched_notify_clock_jump(void);

#ifdef __cplusplus
}
#endif

#endif  // SCHED_H_
