#ifndef LED_H_
#define LED_H_

#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Faixa aceita para a frequência do pisca. Público porque quem monta um comando (CLI, sched)
 * precisa validar ANTES de aceitar — senão um agendamento inválido só falharia no disparo.
 */
#define LED_BLINK_MIN_HZ 1
#define LED_BLINK_MAX_HZ 50

/** Configura o GPIO e sobe a task consumidora da fila do LED. */
esp_err_t led_init(void);

/**
 * Enfileira um comando para a task do LED. Não bloqueia o chamador.
 *
 * `blink_hz` e `blink_ms` só são lidos quando `action == ACT_BLINK`; zero em qualquer um deles
 * significa "usar o padrão do componente" — 5 Hz (100 ms aceso / 100 ms apagado) por 3 s.
 * Um comando novo sempre preempta o pisca em andamento.
 *
 * Retorna ESP_ERR_INVALID_ARG se a ação for inválida ou blink_hz passar de 50 Hz.
 */
esp_err_t led_cmd(sched_action_t action, uint32_t blink_hz, uint32_t blink_ms);

#ifdef __cplusplus
}
#endif

#endif  // LED_H_
