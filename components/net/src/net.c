#include "net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

#define NET_NVS_NS "net"
#define NET_NVS_SSID "ssid"
#define NET_NVS_PASS "pass"

#define NET_CONNECTED_BIT BIT0
#define NET_FAIL_BIT BIT1

#define NET_MAX_RETRIES 5 /* por tentativa de associação; depois disso, desiste e reporta */

static const char *TAG = "net";

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_started;
static int s_retries;
static char s_ssid[NET_SSID_MAXLEN];

/*
 * Roda na task do event loop, não na nossa. Só mexe no event group e pede reconexão — nada de
 * bloquear aqui, senão todo o sistema de eventos do IDF trava atrás deste handler.
 */
static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, NET_CONNECTED_BIT);
        if (s_retries < NET_MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "desconectado, tentativa %d/%d", s_retries, NET_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "falha ao associar em '%s' apos %d tentativas", s_ssid, NET_MAX_RETRIES);
            xEventGroupSetBits(s_events, NET_FAIL_BIT);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "conectado em '%s', ip " IPSTR, s_ssid, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, NET_CONNECTED_BIT);
    }
}

/* --- credenciais em NVS ---------------------------------------------------------------------- */

static esp_err_t net_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NET_NVS_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NET_NVS_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t net_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err; /* ESP_ERR_NVS_NOT_FOUND quando nunca se salvou nada */
    }
    size_t n = ssid_cap;
    err = nvs_get_str(h, NET_NVS_SSID, ssid, &n);
    if (err == ESP_OK) {
        n = pass_cap;
        err = nvs_get_str(h, NET_NVS_PASS, pass, &n);
    }
    nvs_close(h);
    return err;
}

/* --- associação ------------------------------------------------------------------------------ */

static esp_err_t net_apply(const char *ssid, const char *pass, uint32_t timeout_ms) {
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    xEventGroupClearBits(s_events, NET_CONNECTED_BIT | NET_FAIL_BIT);
    s_retries = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_started) {
        /* O start dispara STA_START, e o handler chama esp_wifi_connect(). */
        err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
        s_started = true;
    } else {
        esp_wifi_disconnect();
        esp_wifi_connect();
    }

    if (timeout_ms == 0) {
        return ESP_OK; /* fire-and-forget: usado no boot para não segurar o app_main */
    }

    const EventBits_t bits = xEventGroupWaitBits(s_events, NET_CONNECTED_BIT | NET_FAIL_BIT,
                                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & NET_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & NET_FAIL_BIT) {
        /*
         * ESP_FAIL genérico de propósito: devolver ESP_ERR_WIFI_* obrigaria todo chamador a
         * incluir esp_wifi.h só para ler o retorno. O net.h é o contrato; o driver fica aqui.
         */
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t net_init(void) {
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = já existia */
        ESP_LOGE(TAG, "esp_event_loop_create_default falhou: %s", esp_err_to_name(err));
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta falhou");
        return ESP_FAIL;
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL,
                                              NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_event_handler,
                                                  NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "registro de handler falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode falhou: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Reconecta sozinho ao que já foi provisionado. Sem timeout: o boot não pode ficar preso
     * esperando um AP que pode nem existir mais — quem quiser saber usa `wifi status`.
     */
    char ssid[NET_SSID_MAXLEN];
    char pass[NET_PASS_MAXLEN];
    if (net_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        ESP_LOGI(TAG, "credencial salva encontrada, conectando a '%s'", ssid);
        net_apply(ssid, pass, 0);
    } else {
        ESP_LOGI(TAG, "sem credencial salva (use 'wifi <ssid> <senha>')");
    }
    return ESP_OK;
}

esp_err_t net_connect(const char *ssid, const char *pass, uint32_t timeout_ms) {
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= NET_SSID_MAXLEN || (pass != NULL && strlen(pass) >= NET_PASS_MAXLEN)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *p = (pass != NULL) ? pass : "";
    const esp_err_t err = net_apply(ssid, p, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    /* Só persiste o que comprovadamente funcionou: senão o boot seguinte tentaria lixo. */
    const esp_err_t save_err = net_save(ssid, p);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "conectado, mas falhou ao salvar credencial: %s", esp_err_to_name(save_err));
    }
    return ESP_OK;
}

esp_err_t net_forget(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        err = nvs_commit(h);
        nvs_close(h);
    }
    if (s_started) {
        esp_wifi_disconnect();
    }
    xEventGroupClearBits(s_events, NET_CONNECTED_BIT);
    s_ssid[0] = '\0';
    return err;
}

bool net_is_connected(void) {
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & NET_CONNECTED_BIT) != 0;
}

esp_err_t net_get_ip(char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!net_is_connected() || s_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip;
    const esp_err_t err = esp_netif_get_ip_info(s_netif, &ip);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(buf, cap, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

const char *net_ssid(void) { return s_ssid; }
