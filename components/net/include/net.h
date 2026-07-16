#ifndef NET_H_
#define NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_SSID_MAXLEN 33 /* 32 chars + NUL, limite do 802.11 */
#define NET_PASS_MAXLEN 65 /* 64 chars + NUL */
#define NET_CONNECT_TIMEOUT_MS 15000

/**
 * Sobe netif, event loop e o driver Wi-Fi em modo STA. Se houver credencial salva em NVS,
 * dispara a conexão em background — o boot não espera a rede.
 */
esp_err_t net_init(void);

/**
 * Conecta a `ssid`/`pass` e, ao ter sucesso, salva as credenciais em NVS.
 * `timeout_ms` = 0 não espera (fire-and-forget).
 *
 * Retornos: ESP_OK conectado | ESP_ERR_TIMEOUT não associou a tempo | ESP_FAIL associação
 * recusada (senha errada / SSID inexistente) | ESP_ERR_INVALID_SIZE ssid ou senha longos demais.
 */
esp_err_t net_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

/** Apaga as credenciais salvas e desconecta. */
esp_err_t net_forget(void);

/** True quando associado e com IP. */
bool net_is_connected(void);

/** IP atual em texto. ESP_ERR_INVALID_STATE se não conectado. */
esp_err_t net_get_ip(char *buf, size_t cap);

/** SSID configurado (string vazia se nenhum). Nunca retorna NULL. */
const char *net_ssid(void);

#ifdef __cplusplus
}
#endif

#endif  // NET_H_
