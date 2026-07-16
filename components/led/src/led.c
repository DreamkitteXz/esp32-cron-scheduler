#include "led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LED_GPIO 2 /* LED onboard da DevKit */

#define LED_TASK_STACK 2048
#define LED_TASK_PRIO 6 /* acima do scheduler: o pisca tem prazo, o tick de 1 Hz não */
#define LED_QUEUE_LEN 8

/* Premissa: pisca 5 Hz = 100 ms aceso / 100 ms apagado, por 3 s (ver README). */
#define LED_BLINK_DEFAULT_HZ 5
#define LED_BLINK_DEFAULT_MS 3000
/* LED_BLINK_MIN_HZ / LED_BLINK_MAX_HZ vêm de led.h: a CLI valida com os mesmos limites. */

static const char *TAG = "led";

typedef struct {
    sched_action_t action;
    uint32_t blink_hz; /* 0 = usar o default */
    uint32_t blink_ms; /* 0 = usar o default */
} led_msg_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;

/* Estado do pisca. Tocado SÓ pela led_task, então não precisa de lock. */
typedef struct {
    bool active;
    bool level;
    TickType_t half_period; /* ticks entre dois toggles */
    TickType_t remaining;   /* ticks restantes de pisca; conta para baixo */
} blink_state_t;

static void led_apply(bool level) { gpio_set_level(LED_GPIO, level ? 1 : 0); }

/* Traduz um comando novo em estado, aplicando-o já. Devolve o timeout do próximo receive. */
static TickType_t led_start(const led_msg_t *msg, blink_state_t *b) {
    switch (msg->action) {
        case ACT_ON:
            b->active = false;
            b->level = true;
            led_apply(true);
            return portMAX_DELAY;

        case ACT_OFF:
            b->active = false;
            b->level = false;
            led_apply(false);
            return portMAX_DELAY;

        case ACT_BLINK:
        default: {
            const uint32_t hz = (msg->blink_hz != 0) ? msg->blink_hz : LED_BLINK_DEFAULT_HZ;
            const uint32_t ms = (msg->blink_ms != 0) ? msg->blink_ms : LED_BLINK_DEFAULT_MS;

            b->active = true;
            b->level = true;
            b->half_period = pdMS_TO_TICKS(500 / hz); /* meio período */
            if (b->half_period == 0) {
                b->half_period = 1; /* nunca 0: viraria laço ocupado a 100% de CPU */
            }
            b->remaining = pdMS_TO_TICKS(ms);
            led_apply(true);
            return b->half_period;
        }
    }
}

/*
 * Um passo da máquina de estados, chamado quando o receive expirou (= chegou a hora do toggle).
 * A duração é contada para baixo em vez de comparada com xTaskGetTickCount(), porque o contador
 * de ticks estoura (~49 dias a 1 kHz) e a comparação de instantes quebraria na virada.
 */
static TickType_t led_step(blink_state_t *b) {
    if (!b->active) {
        return portMAX_DELAY; /* defensivo: sem pisca não há passo */
    }
    if (b->remaining <= b->half_period) {
        b->active = false;
        b->level = false;
        led_apply(false); /* o pisca sempre termina apagado: estado final previsível */
        return portMAX_DELAY;
    }
    b->remaining -= b->half_period;
    b->level = !b->level;
    led_apply(b->level);
    return b->half_period;
}

static void led_task(void *arg) {
    (void)arg;

    blink_state_t blink = {0};
    TickType_t wait = portMAX_DELAY;
    led_msg_t msg;

    for (;;) {
        /*
         * O timeout do receive É o instante do próximo toggle (portMAX_DELAY com o LED estático).
         * É isto que faz um comando novo PREEMPTAR o pisca em andamento: ou chega mensagem e o
         * estado troca na hora, ou o timeout vence e a máquina dá um passo. Sem vTaskDelay
         * bloqueante (que ignoraria a fila) e sem um timer extra só para o pisca.
         */
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            wait = led_start(&msg, &blink);
        } else {
            wait = led_step(&blink);
        }
    }
}

esp_err_t led_init(void) {
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) falhou: %s", LED_GPIO, esp_err_to_name(err));
        return err;
    }
    led_apply(false);

    s_queue = xQueueCreate(LED_QUEUE_LEN, sizeof(led_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(led_task, "led", LED_TASK_STACK, NULL, LED_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate falhou");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t led_cmd(sched_action_t action, uint32_t blink_hz, uint32_t blink_ms) {
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (action != ACT_ON && action != ACT_OFF && action != ACT_BLINK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (blink_hz > LED_BLINK_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }

    const led_msg_t msg = {.action = action, .blink_hz = blink_hz, .blink_ms = blink_ms};

    /*
     * Send não bloqueante: o scheduler chama isto logo após soltar o mutex da tabela e não pode
     * ficar preso aqui — atrasaria o tick seguinte. Fila cheia = comando descartado com aviso.
     */
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "fila cheia, comando descartado");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
