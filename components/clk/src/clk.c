#include "clk.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sched.h"

#define CLK_SYNCED_BIT BIT0

static const char *TAG = "clk";

static EventGroupHandle_t s_events;

esp_err_t clk_init(void) {
    // Não é lógica de negócio: fixa o fuso para que localtime() já devolva hora de São Paulo e o
    // CRON case contra a hora local, não UTC.
    setenv("TZ", CLK_TZ, 1);
    tzset();

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate falhou");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t clk_set_manual(const char *datetime) {
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO(dev): parsear "AAAA-MM-DD HH:MM:SS" em struct tm, converter com mktime() (hora local),
    // aplicar com settimeofday(), setar CLK_SYNCED_BIT e chamar sched_notify_clock_jump() — sem
    // isso, um ajuste para o futuro faz o scheduler varrer todos os segundos pulados de uma vez.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t clk_ntp_sync(uint32_t timeout_ms) {
    (void)timeout_ms;

    // TODO(dev): subir a rede (Wi-Fi/esp_netif), configurar o SNTP (esp_sntp_init com
    // "pool.ntp.org"), esperar CLK_SYNCED_BIT em s_events por timeout_ms e, ao sincronizar,
    // chamar sched_notify_clock_jump(). O callback de sync do SNTP só seta o bit.
    return ESP_ERR_NOT_SUPPORTED;
}

bool clk_is_synced(void) {
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & CLK_SYNCED_BIT) != 0;
}
