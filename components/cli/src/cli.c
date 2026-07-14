#include "cli.h"

#include "clk.h"
#include "esp_console.h"
#include "esp_log.h"
#include "led.h"
#include "sched.h"

#define CLI_TASK_STACK 4096
#define CLI_TASK_PRIO 3 /* a mais baixa das três: digitar no console nunca atrasa um disparo */
#define CLI_PROMPT "cron> "
#define CLI_MAX_CMDLINE 128

static const char *TAG = "cli";

static int cmd_add(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // TODO(dev): uso `add "<expr cron>" <on|off|blink>`. Validar argc, mapear a ação, chamar
    // sched_add() e imprimir o id criado — ou a mensagem de cron_strerror() se a expressão for
    // inválida. A CLI só enfileira: quem mexe na tabela é a task do scheduler.
    return 1;
}

static int cmd_del(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // TODO(dev): uso `del <id>`. Converter o id, chamar sched_del() e reportar ESP_ERR_NOT_FOUND
    // como "id inexistente".
    return 1;
}

static int cmd_list(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // TODO(dev): chamar sched_snapshot() para um buffer local de SCHED_MAX_ENTRIES e imprimir a
    // tabela (id, expressão, ação, último disparo).
    return 1;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // TODO(dev): chamar sched_clear() e confirmar quantos agendamentos foram removidos.
    return 1;
}

static int cmd_time(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // TODO(dev): uso `time now` | `time set "AAAA-MM-DD HH:MM:SS"` | `time sync`. Despachar para
    // clk_set_manual() / clk_ntp_sync() / imprimir a hora local corrente e clk_is_synced().
    return 1;
}

static int cmd_led(int argc, char **argv) {
    (void)argc;
    (void)argv;
    // TODO(dev): uso `led <on|off|blink>`. Atalho manual que chama led_cmd() direto, sem passar
    // pelo scheduler — útil para testar o hardware.
    return 1;
}

esp_err_t cli_init(void) {
    static const esp_console_cmd_t cmds[] = {
        {.command = "add", .help = "add \"<expr cron>\" <on|off|blink>", .func = &cmd_add},
        {.command = "del", .help = "del <id>", .func = &cmd_del},
        {.command = "list", .help = "lista os agendamentos ativos", .func = &cmd_list},
        {.command = "clear", .help = "remove todos os agendamentos", .func = &cmd_clear},
        {.command = "time", .help = "time now | set \"...\" | sync", .func = &cmd_time},
        {.command = "led", .help = "led <on|off|blink>", .func = &cmd_led},
    };

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = CLI_PROMPT;
    repl_cfg.max_cmdline_length = CLI_MAX_CMDLINE;
    repl_cfg.task_stack_size = CLI_TASK_STACK;
    repl_cfg.task_priority = CLI_TASK_PRIO;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart falhou: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registro do comando '%s' falhou: %s", cmds[i].command,
                     esp_err_to_name(err));
            return err;
        }
    }
    ESP_ERROR_CHECK(esp_console_register_help_command());

    return esp_console_start_repl(repl);
}
