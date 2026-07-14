#ifndef LED_H_
#define LED_H_

#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configura o GPIO e sobe a task consumidora da fila do LED. */
esp_err_t led_init(void);

/**
 * Enfileira um comando para a task do LED. Não bloqueia o chamador.
 *
 * `blink_hz` e `blink_ms` só são lidos quando `action == ACT_BLINK`; zero significa "usar o
 * padrão do componente" (LED_BLINK_DEFAULT_HZ / LED_BLINK_DEFAULT_MS).
 */
esp_err_t led_cmd(sched_action_t action, uint32_t blink_hz, uint32_t blink_ms);

#ifdef __cplusplus
}
#endif

#endif  // LED_H_
