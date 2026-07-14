#ifndef CLI_H_
#define CLI_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registra os comandos no esp_console e sobe o REPL sobre a UART. */
esp_err_t cli_init(void);

#ifdef __cplusplus
}
#endif

#endif  // CLI_H_
