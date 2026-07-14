#ifndef CLK_H_
#define CLK_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Timezone POSIX de America/Sao_Paulo. UTC-3 fixo: o horário de verão foi extinto em 2019. */
#define CLK_TZ "<-03>3"

/** Fixa o TZ do processo e cria o event group de sincronismo. Não fala com a rede. */
esp_err_t clk_init(void);

/** Ajuste manual do relógio. `datetime` no formato "AAAA-MM-DD HH:MM:SS" (hora local). */
esp_err_t clk_set_manual(const char *datetime);

/** Dispara o SNTP e bloqueia até sincronizar ou estourar `timeout_ms`. */
esp_err_t clk_ntp_sync(uint32_t timeout_ms);

/** True se o relógio já foi acertado (por NTP ou manualmente). */
bool clk_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif  // CLK_H_
