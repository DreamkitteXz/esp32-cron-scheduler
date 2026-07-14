#include "sched.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "led.h"

#define SCHED_TASK_STACK 3072
#define SCHED_TASK_PRIO 5 /* abaixo do LED, acima da CLI */
#define SCHED_CMD_QUEUE_LEN 4
#define SCHED_TICK_MS 1000 /* resolução do CRON é 1 s */
#define SCHED_REPLY_TIMEOUT_MS 500
#define SCHED_CLOCK_JUMP_S 5 /* salto maior que isto -> rebaseline, sem varrer o buraco */

static const char *TAG = "sched";

/* --- Transação CLI -> task ------------------------------------------------------------------
 * A CLI nunca mexe na tabela: ela preenche um sched_cmd_t, enfileira, acorda a task e espera a
 * resposta na fila de reply com timeout. A task do scheduler é a única escritora da tabela; o
 * mutex protege apenas o snapshot (leitura) contra a escrita da própria task.
 */
typedef enum {
    CMD_ADD,
    CMD_DEL,
    CMD_CLEAR,
    CMD_SNAPSHOT,
} sched_cmd_type_t;

typedef struct {
    sched_cmd_type_t type;
    const char *expr;      /* CMD_ADD */
    sched_action_t action; /* CMD_ADD */
    int id;                /* CMD_DEL */
    sched_entry_t *buf;    /* CMD_SNAPSHOT */
    size_t cap;            /* CMD_SNAPSHOT */
} sched_cmd_t;

typedef struct {
    esp_err_t err;
    int id;       /* CMD_ADD */
    size_t count; /* CMD_SNAPSHOT */
} sched_rsp_t;

static sched_entry_t s_table[SCHED_MAX_ENTRIES];
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_rsp_queue;
static TaskHandle_t s_task;
static TimerHandle_t s_timer;
static time_t s_last_eval;

/*
 * Roda na Timer Service task. Regra: só notifica. Nada de mutex, printf ou bloqueio aqui —
 * segurar a Timer Service task atrasaria todos os outros timers do sistema.
 */
static void sched_timer_cb(TimerHandle_t timer) {
    (void)timer;
    xTaskNotifyGive(s_task);
}

static void sched_task(void *arg) {
    (void)arg;

    for (;;) {
        // Uma única fonte de wakeup: a notificação. O timer de 1 Hz dá o tick, e quem enfileira
        // um comando também notifica, então a CLI não espera o próximo segundo para ser atendida.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sched_cmd_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            // TODO(dev): executar o comando (add/del/clear/snapshot) mutando a tabela sob o mutex
            // e devolver um sched_rsp_t em s_rsp_queue. Esta task é a única escritora.
            (void)cmd;
        }

        // TODO(dev): avaliar a tabela para o segundo corrente: sob o mutex, apenas comparar com
        // cron_match() e copiar as ações a disparar para um vetor local; soltar o mutex e só
        // então chamar led_cmd() para cada uma. Seção crítica curta, sem I/O sob o lock.
        // Atualizar s_last_eval e o last_fire das entradas que dispararam.
    }
}

esp_err_t sched_init(void) {
    memset(s_table, 0, sizeof(s_table));
    for (int i = 0; i < SCHED_MAX_ENTRIES; i++) {
        s_table[i].id = -1; /* -1 = slot livre */
    }
    s_last_eval = time(NULL);

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex falhou");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(SCHED_CMD_QUEUE_LEN, sizeof(sched_cmd_t));
    s_rsp_queue = xQueueCreate(1, sizeof(sched_rsp_t));
    if (s_cmd_queue == NULL || s_rsp_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(sched_task, "sched", SCHED_TASK_STACK, NULL, SCHED_TASK_PRIO, &s_task) !=
        pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    s_timer = xTimerCreate("sched1hz", pdMS_TO_TICKS(SCHED_TICK_MS), pdTRUE, NULL, sched_timer_cb);
    if (s_timer == NULL) {
        ESP_LOGE(TAG, "xTimerCreate falhou");
        return ESP_ERR_NO_MEM;
    }
    if (xTimerStart(s_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "xTimerStart falhou");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t sched_add(const char *cron_expr, sched_action_t action, int *out_id) {
    if (cron_expr == NULL || out_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)action;
    *out_id = -1;

    // TODO(dev): montar um sched_cmd_t{CMD_ADD}, xQueueSend, xTaskNotifyGive(s_task) e esperar em
    // s_rsp_queue por SCHED_REPLY_TIMEOUT_MS. A task valida com cron_parse() e ocupa o slot livre.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sched_del(int id) {
    if (id < 0 || id >= SCHED_MAX_ENTRIES) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO(dev): mesma transação de sched_add, com CMD_DEL. ESP_ERR_NOT_FOUND se o slot já estava
    // livre.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sched_clear(void) {
    // TODO(dev): mesma transação, com CMD_CLEAR.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sched_snapshot(sched_entry_t *buf, size_t cap, size_t *out_count) {
    if (buf == NULL || out_count == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    // TODO(dev): mesma transação, com CMD_SNAPSHOT: a task copia as entradas ativas para `buf`
    // sob o mutex (copy-out; o chamador nunca vê ponteiro para a tabela) e devolve a contagem.
    return ESP_ERR_NOT_SUPPORTED;
}

void sched_notify_clock_jump(void) {
    // TODO(dev): se (now - s_last_eval) > SCHED_CLOCK_JUMP_S, fazer s_last_eval = now (rebaseline)
    // em vez de avaliar os segundos pulados. Sem isso, um `time set` para o futuro varreria o
    // intervalo inteiro e dispararia uma avalanche de ações de uma vez.
    // Cuidado: s_last_eval é lido pela task do scheduler; proteger com o mutex ou trocar por um
    // flag atômico consumido pela própria task.
}
