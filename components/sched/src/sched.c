#include "sched.h"

#include <string.h>
#include <time.h>

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

/* --- Modelo de concorrência -----------------------------------------------------------------
 *
 * A tabela tem UMA ÚNICA escritora: a task do scheduler. A CLI nunca a muta — preenche um
 * sched_cmd_t, enfileira, acorda a task e espera a resposta com timeout. Isso mantém invariantes
 * (achar slot livre + ocupá-lo) atômicas por construção, sem a CLI segurar lock nenhum, e impede
 * que uma mutação se intercale no meio de uma avaliação.
 *
 * O mutex (s_table_mutex) existe por um motivo só: sched_snapshot lê a tabela no contexto do
 * CHAMADOR, então precisa se proteger contra a escrita da task.
 *
 * O s_api_mutex serializa os chamadores da API, o que torna seguro haver uma única fila de
 * resposta compartilhada (senão a resposta de um chamador poderia ser lida por outro).
 */
typedef enum {
    CMD_ADD,
    CMD_DEL,
    CMD_CLEAR,
} sched_cmd_type_t;

/*
 * Comando copiado POR VALOR na fila (nada de ponteiro para o buffer do chamador). Se o chamador
 * estourar o timeout e retornar, seu buffer some — mas a task pode processar o comando depois.
 * Um ponteiro aqui viraria dangling nesse instante; a cópia elimina a classe do bug de vez.
 */
typedef struct {
    sched_cmd_type_t type;
    cron_expr_t expr;                 /* CMD_ADD: já compilada pelo chamador */
    char expr_str[SCHED_EXPR_MAXLEN]; /* CMD_ADD */
    sched_action_t action;            /* CMD_ADD */
    uint32_t blink_hz;                /* CMD_ADD */
    uint32_t blink_ms;                /* CMD_ADD */
    int id;                           /* CMD_DEL */
} sched_cmd_t;

typedef struct {
    esp_err_t err;
    int id;       /* CMD_ADD: slot criado */
    size_t count; /* CMD_CLEAR: quantos foram removidos */
} sched_rsp_t;

static sched_entry_t s_table[SCHED_MAX_ENTRIES];
static SemaphoreHandle_t s_table_mutex;
static SemaphoreHandle_t s_api_mutex;
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

/* --- Executado SÓ na task do scheduler ------------------------------------------------------ */

static int find_free_slot(void) {
    for (int i = 0; i < SCHED_MAX_ENTRIES; i++) {
        if (s_table[i].id < 0) {
            return i;
        }
    }
    return -1;
}

static sched_rsp_t sched_exec(const sched_cmd_t *cmd) {
    sched_rsp_t rsp = {.err = ESP_OK, .id = -1, .count = 0};

    xSemaphoreTake(s_table_mutex, portMAX_DELAY);
    switch (cmd->type) {
        case CMD_ADD: {
            const int slot = find_free_slot();
            if (slot < 0) {
                rsp.err = ESP_ERR_NO_MEM; /* limite de 10 atingido */
                break;
            }
            s_table[slot].id = slot;
            s_table[slot].action = cmd->action;
            s_table[slot].expr = cmd->expr;
            memcpy(s_table[slot].expr_str, cmd->expr_str, sizeof(s_table[slot].expr_str));
            s_table[slot].blink_hz = cmd->blink_hz;
            s_table[slot].blink_ms = cmd->blink_ms;
            s_table[slot].last_fire = 0;
            rsp.id = slot;
            break;
        }
        case CMD_DEL: {
            if (s_table[cmd->id].id < 0) {
                rsp.err = ESP_ERR_NOT_FOUND;
                break;
            }
            s_table[cmd->id].id = -1;
            break;
        }
        case CMD_CLEAR: {
            for (int i = 0; i < SCHED_MAX_ENTRIES; i++) {
                if (s_table[i].id >= 0) {
                    s_table[i].id = -1;
                    rsp.count++;
                }
            }
            break;
        }
    }
    xSemaphoreGive(s_table_mutex);
    return rsp;
}

/*
 * Avalia um segundo específico. Seção crítica CURTA de propósito: sob o mutex só comparamos e
 * copiamos as ações para um vetor local. led_cmd() fica FORA do lock — ele enfileira e loga, e
 * segurar o mutex durante isso bloquearia sched_snapshot (a CLI) sem necessidade.
 */
static void evaluate_second(time_t when) {
    struct tm tm_when;
    if (localtime_r(&when, &tm_when) == NULL) {
        return;
    }

    /* Vetor local: o que precisamos levar para fora do lock, e nada além disso. */
    struct {
        sched_action_t action;
        uint32_t blink_hz;
        uint32_t blink_ms;
    } pending[SCHED_MAX_ENTRIES];
    size_t npending = 0;

    xSemaphoreTake(s_table_mutex, portMAX_DELAY);
    for (int i = 0; i < SCHED_MAX_ENTRIES; i++) {
        if (s_table[i].id < 0) {
            continue;
        }
        if (s_table[i].last_fire == when) {
            continue; /* já disparou neste segundo: a task pode acordar mais de uma vez por segundo
                       */
        }
        if (!cron_match(&s_table[i].expr, &tm_when)) {
            continue;
        }
        s_table[i].last_fire = when;
        pending[npending].action = s_table[i].action;
        pending[npending].blink_hz = s_table[i].blink_hz;
        pending[npending].blink_ms = s_table[i].blink_ms;
        npending++;
    }
    xSemaphoreGive(s_table_mutex);

    for (size_t i = 0; i < npending; i++) {
        const esp_err_t err = led_cmd(pending[i].action, pending[i].blink_hz, pending[i].blink_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led_cmd falhou: %s", esp_err_to_name(err));
        }
    }
}

/*
 * O tick de 1 Hz do FreeRTOS conta ticks do RTOS, não segundos do relógio de parede: os dois
 * derivam entre si, então um tick pode escorregar e pular um segundo inteiro. Por isso avaliamos
 * a FAIXA (s_last_eval, now], não só `now`.
 *
 * O teto de SCHED_CLOCK_JUMP_S é o que impede essa varredura de virar avalanche: sem ele, o
 * primeiro sync NTP (que salta de 1970 para hoje) mandaria varrer ~1,7 bilhão de segundos.
 */
static void sched_evaluate(void) {
    const time_t now = time(NULL);

    if (now == s_last_eval) {
        /*
         * Nada novo neste segundo. Cai aqui sempre que a task acorda por um comando da CLI em vez
         * do tick — o caso comum, não uma anomalia: sair silencioso.
         */
        return;
    }
    if (now < s_last_eval) {
        /* Relógio andou para trás: rebaseline, sem reprocessar segundos já avaliados. */
        ESP_LOGI(TAG, "relogio recuou %lld s: rebaseline", (long long)(s_last_eval - now));
        s_last_eval = now;
        return;
    }

    time_t from = s_last_eval + 1;
    if (now - s_last_eval > SCHED_CLOCK_JUMP_S) {
        /* Salto grande (time set/NTP): rebaseline em vez de varrer o buraco e disparar avalanche.
         */
        ESP_LOGI(TAG, "relogio saltou %lld s: rebaseline, %lld segundos ignorados",
                 (long long)(now - s_last_eval), (long long)(now - s_last_eval));
        from = now;
    }

    for (time_t s = from; s <= now; s++) {
        evaluate_second(s);
    }
    s_last_eval = now;
}

static void sched_task(void *arg) {
    (void)arg;

    for (;;) {
        /*
         * Uma única fonte de wakeup: a notificação. O timer de 1 Hz dá o tick, e quem enfileira um
         * comando também notifica, então a CLI é atendida na hora, sem esperar o próximo segundo.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sched_cmd_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            const sched_rsp_t rsp = sched_exec(&cmd);
            /* Sem espera: se o chamador desistiu, a resposta é descartada em vez de travar a task.
             */
            xQueueSend(s_rsp_queue, &rsp, 0);
        }

        sched_evaluate();
    }
}

/* --- API chamada pela CLI (nunca toca a tabela) ---------------------------------------------- */

/*
 * Uma transação completa: serializa contra outros chamadores, enfileira, acorda a task e espera
 * a resposta. O xQueueReset descarta resposta atrasada de uma transação anterior que estourou o
 * timeout — senão ela seria lida como se fosse a nossa.
 */
static esp_err_t sched_transact(const sched_cmd_t *cmd, sched_rsp_t *rsp) {
    if (s_api_mutex == NULL || s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE; /* sched_init não rodou */
    }
    if (xSemaphoreTake(s_api_mutex, pdMS_TO_TICKS(SCHED_REPLY_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    xQueueReset(s_rsp_queue);

    if (xQueueSend(s_cmd_queue, cmd, pdMS_TO_TICKS(SCHED_REPLY_TIMEOUT_MS)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    } else {
        xTaskNotifyGive(s_task);
        if (xQueueReceive(s_rsp_queue, rsp, pdMS_TO_TICKS(SCHED_REPLY_TIMEOUT_MS)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        } else {
            err = rsp->err;
        }
    }

    xSemaphoreGive(s_api_mutex);
    return err;
}

esp_err_t sched_add(const char *cron_expr, sched_action_t action, uint32_t blink_hz,
                    uint32_t blink_ms, int *out_id) {
    if (cron_expr == NULL || out_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_id = -1;
    if (action != ACT_ON && action != ACT_OFF && action != ACT_BLINK) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Valida a frequência aqui também: aceitar um agendamento que só vai falhar no disparo, dali a
     * horas e longe do comando que o criou, é bem pior do que recusá-lo agora. (0 = default.)
     */
    if (blink_hz != 0 && (blink_hz < LED_BLINK_MIN_HZ || blink_hz > LED_BLINK_MAX_HZ)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(cron_expr) >= SCHED_EXPR_MAXLEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    sched_cmd_t cmd = {
        .type = CMD_ADD, .action = action, .blink_hz = blink_hz, .blink_ms = blink_ms, .id = -1};

    /*
     * Parse no contexto do chamador: cron_parse é pura (sem estado compartilhado), então o
     * trabalho fica fora da task do scheduler e uma expressão inválida falha aqui mesmo, sem
     * gastar uma ida e volta na fila.
     */
    const cron_err_t cerr = cron_parse(cron_expr, &cmd.expr);
    if (cerr != CRON_OK) {
        return ESP_ERR_INVALID_ARG; /* a CLI reimprime o motivo via cron_strerror */
    }
    strcpy(cmd.expr_str, cron_expr); /* tamanho já validado acima */

    sched_rsp_t rsp;
    const esp_err_t err = sched_transact(&cmd, &rsp);
    if (err == ESP_OK) {
        *out_id = rsp.id;
        ESP_LOGI(TAG, "agendamento %d criado: \"%s\"", rsp.id, cron_expr);
    }
    return err;
}

esp_err_t sched_del(int id) {
    if (id < 0 || id >= SCHED_MAX_ENTRIES) {
        return ESP_ERR_INVALID_ARG;
    }
    sched_cmd_t cmd = {.type = CMD_DEL, .id = id};
    sched_rsp_t rsp;
    return sched_transact(&cmd, &rsp);
}

esp_err_t sched_clear(size_t *out_removed) {
    sched_cmd_t cmd = {.type = CMD_CLEAR, .id = -1};
    sched_rsp_t rsp;
    const esp_err_t err = sched_transact(&cmd, &rsp);
    if (out_removed != NULL) {
        *out_removed = (err == ESP_OK) ? rsp.count : 0;
    }
    return err;
}

/*
 * Copy-out sob mutex: o chamador recebe uma cópia e nunca um ponteiro para dentro da tabela —
 * assim ele pode formatar/imprimir à vontade sem segurar o lock e sem ler algo que a task esteja
 * mutando no meio.
 */
esp_err_t sched_snapshot(sched_entry_t *buf, size_t cap, size_t *out_count) {
    if (buf == NULL || out_count == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (s_table_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_table_mutex, pdMS_TO_TICKS(SCHED_REPLY_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t n = 0;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < SCHED_MAX_ENTRIES; i++) {
        if (s_table[i].id < 0) {
            continue;
        }
        if (n >= cap) {
            err = ESP_ERR_INVALID_SIZE; /* buffer do chamador é menor que a tabela ativa */
            break;
        }
        buf[n++] = s_table[i];
    }

    xSemaphoreGive(s_table_mutex);
    *out_count = n;
    return err;
}

/*
 * Chamado pelo clk depois de um `time set` ou sync NTP, de outra task.
 *
 * Repare no que esta função NÃO faz: não toca em s_last_eval nem levanta flag compartilhada. A
 * regra de rebaseline (gap > SCHED_CLOCK_JUMP_S) já mora em sched_evaluate, que roda na task dona
 * de s_last_eval — duplicá-la aqui criaria uma escrita concorrente sobre esse estado, exatamente
 * o tipo de corrida que o modelo de dono único existe para evitar.
 *
 * Então isto é só "acorde e olhe o relógio agora": a decisão continua num lugar só, e o efeito é
 * aplicar o rebaseline na hora do ajuste em vez de até 1 s depois, no próximo tick.
 */
void sched_notify_clock_jump(void) {
    if (s_task == NULL) {
        return;
    }
    xTaskNotifyGive(s_task);
}

esp_err_t sched_init(void) {
    memset(s_table, 0, sizeof(s_table));
    for (int i = 0; i < SCHED_MAX_ENTRIES; i++) {
        s_table[i].id = -1; /* -1 = slot livre */
    }
    s_last_eval = time(NULL);

    s_table_mutex = xSemaphoreCreateMutex();
    s_api_mutex = xSemaphoreCreateMutex();
    if (s_table_mutex == NULL || s_api_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex falhou");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(SCHED_CMD_QUEUE_LEN, sizeof(sched_cmd_t));
    s_rsp_queue = xQueueCreate(1, sizeof(sched_rsp_t));
    if (s_cmd_queue == NULL || s_rsp_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    /* A task precisa existir antes do timer: o callback do timer notifica s_task. */
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
