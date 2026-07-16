#include <stdlib.h>

#include "cli.h"
#include "clk.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "led.h"
#include "net.h"
#include "nvs_flash.h"
#include "sched.h"

static const char *TAG = "main";

/*
 * Sem objeto RTOS criado às escondidas: se uma fila, task, mutex ou timer não nasce, o firmware
 * não sobe fingindo que está bem. Cada *_init() devolve ESP_ERR_NO_MEM nesse caso e aqui o boot
 * é abortado com o nome do módulo — bem mais barato de diagnosticar do que um crash aleatório
 * dois minutos depois.
 */
#define INIT_OR_ABORT(fn)                                               \
    do {                                                                \
        esp_err_t err_ = (fn);                                          \
        if (err_ != ESP_OK) {                                           \
            ESP_LOGE(TAG, "%s falhou: %s", #fn, esp_err_to_name(err_)); \
            abort();                                                    \
        }                                                               \
        ESP_LOGI(TAG, "%s ok", #fn);                                    \
    } while (0)

/*
 * O NVS guarda a credencial do Wi-Fi e é pré-requisito do driver. Partição corrompida ou de uma
 * versão antiga é recuperável: apaga e recria. Só falha de verdade se nem isso funcionar.
 */
static esp_err_t nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS ilegivel (%s), reformatando", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void) {
    /*
     * Ordem importa: nvs antes de net (credenciais), led antes de sched (o scheduler enfileira
     * para o LED), sched antes de clk (clk avisa o scheduler sobre saltos de relógio) e a CLI por
     * último, quando tudo já responde.
     */
    INIT_OR_ABORT(nvs_init());
    INIT_OR_ABORT(net_init());
    INIT_OR_ABORT(led_init());
    INIT_OR_ABORT(sched_init());
    INIT_OR_ABORT(clk_init());
    INIT_OR_ABORT(cli_init());

    ESP_LOGI(TAG, "pronto. digite 'help' no console.");
}
