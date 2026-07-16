#include "clk.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sched.h"

#define CLK_SYNCED_BIT BIT0

static const char *TAG = "clk";

static EventGroupHandle_t s_events;
static bool s_sntp_running;

esp_err_t clk_init(void) {
    /*
     * Fixa o fuso para que localtime_r() já devolva hora de São Paulo: o CRON casa contra a hora
     * local, não UTC, então isto tem que valer antes de qualquer avaliação.
     */
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
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int year, mon, day, hour, min, sec;
    if (sscanf(datetime, "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        min < 0 || min > 59 || sec < 0 || sec > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm t = {
        .tm_year = year - 1900,
        .tm_mon = mon - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = min,
        .tm_sec = sec,
        .tm_isdst = -1, /* deixa a libc resolver o fuso */
    };
    const time_t epoch = mktime(&t); /* interpreta como hora LOCAL, pois TZ já está setado */
    if (epoch == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * mktime normaliza data inexistente em silêncio (31/02 vira 03/03). Comparar de volta é o que
     * transforma isso num erro reportado em vez de um relógio ajustado para outro dia.
     */
    if (t.tm_year != year - 1900 || t.tm_mon != mon - 1 || t.tm_mday != day) {
        return ESP_ERR_INVALID_ARG;
    }

    const struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday falhou");
        return ESP_FAIL;
    }

    xEventGroupSetBits(s_events, CLK_SYNCED_BIT);
    ESP_LOGI(TAG, "relogio ajustado manualmente para %s", datetime);

    /* Avisa o scheduler ANTES de retornar: sem isto, o salto vira avalanche de disparos. */
    sched_notify_clock_jump();
    return ESP_OK;
}

esp_err_t clk_ntp_sync(uint32_t timeout_ms) {
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sntp_running) {
        return ESP_ERR_INVALID_STATE; /* uma sincronização por vez */
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CLK_NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init falhou: %s", esp_err_to_name(err));
        return err;
    }
    s_sntp_running = true;

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));

    /* Sempre desmonta: sem isto, uma segunda chamada falharia com "already initialized". */
    esp_netif_sntp_deinit();
    s_sntp_running = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP nao sincronizou: %s", esp_err_to_name(err));
        return err;
    }

    xEventGroupSetBits(s_events, CLK_SYNCED_BIT);
    ESP_LOGI(TAG, "relogio sincronizado por NTP");
    sched_notify_clock_jump();
    return ESP_OK;
}

bool clk_is_synced(void) {
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & CLK_SYNCED_BIT) != 0;
}
