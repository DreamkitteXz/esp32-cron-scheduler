#include "led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LED_GPIO 2 /* LED onboard da DevKit */

#define LED_TASK_STACK 2048
#define LED_TASK_PRIO 6 /* acima do scheduler: o pisca não pode ser atrasado por ele */
#define LED_QUEUE_LEN 8

/* Padrões do pisca quando o agendamento não especifica (ver README, "Parâmetros do pisca"). */
#define LED_BLINK_DEFAULT_HZ 2
#define LED_BLINK_DEFAULT_MS 3000

static const char *TAG = "led";

typedef struct {
    sched_action_t action;
    uint32_t blink_hz;
    uint32_t blink_ms;
} led_msg_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static void led_task(void *arg) {
    (void)arg;

    led_msg_t msg;
    TickType_t wait = portMAX_DELAY;

    for (;;) {
        // O timeout do receive é o instante do próximo toggle do pisca (portMAX_DELAY quando o
        // LED está estático). É isso que faz um comando novo preemptar um pisca em andamento:
        // ou chega mensagem, ou vence o timeout e a máquina de estados avança um passo.
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            // TODO(dev): trocar o estado corrente (ON / OFF / BLINK) a partir de msg, aplicar o
            // nível no GPIO e recalcular `wait`: portMAX_DELAY em ON/OFF, meio período em BLINK.
            (void)msg;
        } else {
            // TODO(dev): timeout -> um passo da máquina de estados do pisca: inverter o nível do
            // GPIO e, se a duração total já expirou, voltar para OFF e `wait = portMAX_DELAY`.
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
    gpio_set_level(LED_GPIO, 0);

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

    const led_msg_t msg = {
        .action = action,
        .blink_hz = (blink_hz != 0) ? blink_hz : LED_BLINK_DEFAULT_HZ,
        .blink_ms = (blink_ms != 0) ? blink_ms : LED_BLINK_DEFAULT_MS,
    };

    // Send não bloqueante: o scheduler chama isto já fora do mutex e não pode ficar preso aqui.
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "fila cheia, comando descartado");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
