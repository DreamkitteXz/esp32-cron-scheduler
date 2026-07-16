#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clk.h"
#include "esp_console.h"
#include "esp_log.h"
#include "led.h"
#include "net.h"
#include "sched.h"

#define CLI_TASK_STACK 4096
#define CLI_TASK_PRIO 3 /* a mais baixa das três: digitar no console nunca atrasa um disparo */
#define CLI_PROMPT "cron> "
#define CLI_MAX_CMDLINE 128

static const char *TAG = "cli";

/*
 * Parsing feito com argc/argv cru, e não com argtable, de propósito: estes comandos são do tipo
 * "subcomando + posicionais" (`sched add <expr> <acao>`), não do tipo flag (`--hz=5`), que é onde
 * o argtable ganha. Para subcomando, o argtable exigiria um parser por variante e ficaria mais
 * longo que o switch abaixo.
 *
 * As aspas são resolvidas antes de chegar aqui: o esp_console_split_argv trata "..." como um
 * argumento único, então `sched add "0 30 8 * * 1-5" liga` chega com argv[2] inteiro.
 */

/* --- Conversões nome <-> enum ---------------------------------------------------------------- */

static bool parse_action(const char *s, sched_action_t *out) {
    if (strcmp(s, "liga") == 0) {
        *out = ACT_ON;
        return true;
    }
    if (strcmp(s, "desliga") == 0) {
        *out = ACT_OFF;
        return true;
    }
    if (strcmp(s, "pisca") == 0) {
        *out = ACT_BLINK;
        return true;
    }
    return false;
}

static const char *action_name(sched_action_t a) {
    switch (a) {
        case ACT_ON:
            return "liga";
        case ACT_OFF:
            return "desliga";
        case ACT_BLINK:
            return "pisca";
        default:
            return "?";
    }
}

/* Converte um argumento em inteiro >= 0; rejeita lixo ("5x") em vez de aceitar o prefixo. */
static bool parse_uint_arg(const char *s, uint32_t *out) {
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/*
 * Parâmetros opcionais do pisca, compartilhados por `sched add ... pisca [hz] [s]` e
 * `led pisca [hz] [s]`. Ausentes => 0 => o componente led aplica o default (5 Hz por 3 s).
 */
static bool parse_blink_args(int argc, char **argv, int first, uint32_t *hz, uint32_t *ms) {
    *hz = 0;
    *ms = 0;
    if (argc > first) {
        /* Valida a faixa aqui, e não só no disparo: um agendamento aceito tem que funcionar. */
        if (!parse_uint_arg(argv[first], hz) || *hz < LED_BLINK_MIN_HZ || *hz > LED_BLINK_MAX_HZ) {
            printf("erro: frequencia do pisca invalida: '%s' (use %d..%d Hz)\n", argv[first],
                   LED_BLINK_MIN_HZ, LED_BLINK_MAX_HZ);
            return false;
        }
    }
    if (argc > first + 1) {
        uint32_t seconds;
        if (!parse_uint_arg(argv[first + 1], &seconds) || seconds == 0) {
            printf("erro: duracao do pisca invalida: '%s' (segundos, > 0)\n", argv[first + 1]);
            return false;
        }
        *ms = seconds * 1000;
    }
    return true;
}

/* --- sched add | del | clear ------------------------------------------------------------------ */

static int cmd_sched_add(int argc, char **argv) {
    /* sched add "<expr cron>" <liga|desliga|pisca> [hz] [seg] */
    if (argc < 4) {
        printf("uso: sched add \"<expr cron>\" <liga|desliga|pisca> [hz] [seg]\n");
        return 1;
    }

    const char *expr = argv[2];

    sched_action_t action;
    if (!parse_action(argv[3], &action)) {
        printf("erro: acao invalida: '%s' (use liga, desliga ou pisca)\n", argv[3]);
        return 1;
    }

    uint32_t hz = 0, ms = 0;
    if (action == ACT_BLINK) {
        if (!parse_blink_args(argc, argv, 4, &hz, &ms)) {
            return 1;
        }
    } else if (argc > 4) {
        printf("erro: '%s' nao aceita parametros extras (so 'pisca' aceita hz e duracao)\n",
               argv[3]);
        return 1;
    }

    /*
     * Valida a expressão aqui só para poder dizer POR QUE ela é inválida: sched_add devolve um
     * esp_err_t genérico, que perderia a mensagem específica do cron_strerror.
     */
    cron_expr_t parsed;
    const cron_err_t cerr = cron_parse(expr, &parsed);
    if (cerr != CRON_OK) {
        printf("erro: expressao cron invalida: %s\n", cron_strerror(cerr));
        printf("      formato: <seg> <min> <hora> <dia> <mes> <dia-semana>\n");
        return 1;
    }

    int id = -1;
    const esp_err_t err = sched_add(expr, action, hz, ms, &id);
    switch (err) {
        case ESP_OK:
            printf("agendamento %d criado\n", id);
            return 0;
        case ESP_ERR_NO_MEM:
            printf("erro: limite de %d agendamentos atingido (use 'sched del' ou 'sched clear')\n",
                   SCHED_MAX_ENTRIES);
            return 1;
        case ESP_ERR_INVALID_SIZE:
            printf("erro: expressao muito longa (max %d caracteres)\n", SCHED_EXPR_MAXLEN - 1);
            return 1;
        default:
            printf("erro: falha ao agendar: %s\n", esp_err_to_name(err));
            return 1;
    }
}

static int cmd_sched_del(int argc, char **argv) {
    if (argc < 3) {
        printf("uso: sched del <id>\n");
        return 1;
    }

    uint32_t id;
    if (!parse_uint_arg(argv[2], &id) || id >= SCHED_MAX_ENTRIES) {
        printf("erro: id invalido: '%s' (use 0..%d)\n", argv[2], SCHED_MAX_ENTRIES - 1);
        return 1;
    }

    const esp_err_t err = sched_del((int)id);
    switch (err) {
        case ESP_OK:
            printf("agendamento %u removido\n", (unsigned)id);
            return 0;
        case ESP_ERR_NOT_FOUND:
            printf("erro: agendamento %u nao existe\n", (unsigned)id);
            return 1;
        default:
            printf("erro: falha ao remover: %s\n", esp_err_to_name(err));
            return 1;
    }
}

static int cmd_sched_clear(void) {
    size_t removed = 0;
    const esp_err_t err = sched_clear(&removed);
    if (err != ESP_OK) {
        printf("erro: falha ao limpar: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("%u agendamento(s) removido(s)\n", (unsigned)removed);
    return 0;
}

static int cmd_sched(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: sched <add|del|clear> ...\n");
        return 1;
    }
    if (strcmp(argv[1], "add") == 0) {
        return cmd_sched_add(argc, argv);
    }
    if (strcmp(argv[1], "del") == 0) {
        return cmd_sched_del(argc, argv);
    }
    if (strcmp(argv[1], "clear") == 0) {
        return cmd_sched_clear();
    }
    printf("erro: subcomando desconhecido: '%s' (use add, del ou clear)\n", argv[1]);
    return 1;
}

/* --- status ---------------------------------------------------------------------------------- */

static void format_last_fire(time_t t, char *buf, size_t cap) {
    if (t == 0) {
        snprintf(buf, cap, "nunca");
        return;
    }
    struct tm tm_t;
    if (localtime_r(&t, &tm_t) == NULL) {
        snprintf(buf, cap, "?");
        return;
    }
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm_t);
}

static int cmd_status(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Buffer local dimensionado para a tabela inteira: o copy-out nunca trunca. */
    sched_entry_t buf[SCHED_MAX_ENTRIES];
    size_t count = 0;
    const esp_err_t err = sched_snapshot(buf, SCHED_MAX_ENTRIES, &count);
    if (err != ESP_OK) {
        printf("erro: falha ao ler agendamentos: %s\n", esp_err_to_name(err));
        return 1;
    }

    char now_str[32] = "?";
    const time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) != NULL) {
        strftime(now_str, sizeof(now_str), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
    }
    printf("hora: %s (relogio %s)\n", now_str,
           clk_is_synced() ? "sincronizado" : "NAO sincronizado");

    if (net_is_connected()) {
        char ip[16] = "?";
        net_get_ip(ip, sizeof(ip));
        printf("rede: '%s' (%s)\n", net_ssid(), ip);
    } else {
        printf("rede: desconectado\n");
    }

    if (count == 0) {
        printf("nenhum agendamento (%d slots livres)\n", SCHED_MAX_ENTRIES);
        return 0;
    }

    printf("\n%-3s %-24s %-8s %-8s %s\n", "ID", "EXPRESSAO", "ACAO", "PISCA", "ULTIMO DISPARO");
    for (size_t i = 0; i < count; i++) {
        /* 32 bytes: cabe o pior caso de dois uint32 por extenso, que é o que o compilador assume.
         */
        char blink[32] = "-";
        if (buf[i].action == ACT_BLINK) {
            const unsigned hz = (unsigned)(buf[i].blink_hz != 0 ? buf[i].blink_hz : 5);
            const unsigned secs =
                (unsigned)((buf[i].blink_ms != 0 ? buf[i].blink_ms : 3000) / 1000);
            snprintf(blink, sizeof(blink), "%uHz/%us", hz, secs);
        }
        char last[32];
        format_last_fire(buf[i].last_fire, last, sizeof(last));
        printf("%-3d %-24s %-8s %-8s %s\n", buf[i].id, buf[i].expr_str, action_name(buf[i].action),
               blink, last);
    }
    printf("\n%u/%d slots em uso\n", (unsigned)count, SCHED_MAX_ENTRIES);
    return 0;
}

/* --- time get | set | sync ------------------------------------------------------------------- */

static int cmd_time(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: time <get|set|sync> ...\n");
        return 1;
    }

    if (strcmp(argv[1], "get") == 0) {
        const time_t now = time(NULL);
        struct tm tm_now;
        if (localtime_r(&now, &tm_now) == NULL) {
            printf("erro: relogio indisponivel\n");
            return 1;
        }
        char s[64];
        strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm_now);
        printf("%s\n", s);
        printf("relogio %s\n",
               clk_is_synced() ? "sincronizado" : "NAO sincronizado (use time set/sync)");
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            printf("uso: time set \"AAAA-MM-DD HH:MM:SS\"\n");
            return 1;
        }
        const esp_err_t err = clk_set_manual(argv[2]);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("erro: data/hora invalida: '%s'\n", argv[2]);
            printf("      formato: \"AAAA-MM-DD HH:MM:SS\" (ex.: \"2026-07-16 08:30:00\")\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("erro: falha ao ajustar o relogio: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("relogio ajustado\n");
        return 0;
    }

    if (strcmp(argv[1], "sync") == 0) {
        /* Checa a rede antes: falhar na hora é melhor que gastar 10 s de timeout para nada. */
        if (!net_is_connected()) {
            printf("erro: sem rede. Use 'wifi <ssid> <senha>' antes de sincronizar,\n");
            printf("      ou acerte o relogio na mao com 'time set'.\n");
            return 1;
        }
        printf("sincronizando com NTP...\n");
        const esp_err_t err = clk_ntp_sync(CLK_NTP_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            printf("erro: NTP nao respondeu em %d ms\n", CLK_NTP_TIMEOUT_MS);
            return 1;
        }
        if (err != ESP_OK) {
            printf("erro: falha no NTP: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("relogio sincronizado por NTP\n");
        return 0;
    }

    printf("erro: subcomando desconhecido: '%s' (use get, set ou sync)\n", argv[1]);
    return 1;
}

/* --- wifi ------------------------------------------------------------------------------------ */

static int cmd_wifi_status(void) {
    if (!net_is_connected()) {
        const char *ssid = net_ssid();
        if (ssid[0] == '\0') {
            printf("wifi: sem credencial (use 'wifi <ssid> <senha>')\n");
        } else {
            printf("wifi: desconectado (tentando '%s')\n", ssid);
        }
        return 0;
    }
    char ip[16] = "?";
    net_get_ip(ip, sizeof(ip));
    printf("wifi: conectado em '%s', ip %s\n", net_ssid(), ip);
    return 0;
}

static int cmd_wifi(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: wifi <ssid> [senha] | wifi status | wifi forget\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        return cmd_wifi_status();
    }

    if (strcmp(argv[1], "forget") == 0) {
        const esp_err_t err = net_forget();
        if (err != ESP_OK) {
            printf("erro: falha ao apagar credencial: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("credencial apagada\n");
        return 0;
    }

    /* wifi <ssid> [senha] — rede aberta se a senha for omitida. */
    const char *ssid = argv[1];
    const char *pass = (argc > 2) ? argv[2] : "";

    printf("conectando a '%s'...\n", ssid);
    const esp_err_t err = net_connect(ssid, pass, NET_CONNECT_TIMEOUT_MS);
    switch (err) {
        case ESP_OK:
            return cmd_wifi_status();
        case ESP_ERR_TIMEOUT:
            printf("erro: nao associou em %d ms (SSID no alcance?)\n", NET_CONNECT_TIMEOUT_MS);
            return 1;
        case ESP_FAIL:
            printf("erro: falha ao associar (senha errada ou SSID inexistente)\n");
            return 1;
        case ESP_ERR_INVALID_SIZE:
            printf("erro: ssid (max %d) ou senha (max %d) longos demais\n", NET_SSID_MAXLEN - 1,
                   NET_PASS_MAXLEN - 1);
            return 1;
        default:
            printf("erro: falha no wifi: %s\n", esp_err_to_name(err));
            return 1;
    }
}

/* --- led (atalho manual, sem passar pelo scheduler) ------------------------------------------- */

static int cmd_led(int argc, char **argv) {
    if (argc < 2) {
        printf("uso: led <liga|desliga|pisca> [hz] [seg]\n");
        return 1;
    }

    sched_action_t action;
    if (!parse_action(argv[1], &action)) {
        printf("erro: acao invalida: '%s' (use liga, desliga ou pisca)\n", argv[1]);
        return 1;
    }

    uint32_t hz = 0, ms = 0;
    if (action == ACT_BLINK && !parse_blink_args(argc, argv, 2, &hz, &ms)) {
        return 1;
    }

    const esp_err_t err = led_cmd(action, hz, ms);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("erro: frequencia fora da faixa suportada (1..50 Hz)\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("erro: falha ao acionar o led: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("led: %s\n", action_name(action));
    return 0;
}

/* --- registro -------------------------------------------------------------------------------- */

esp_err_t cli_init(void) {
    static const esp_console_cmd_t cmds[] = {
        {.command = "sched",
         .help = "sched add \"<expr cron>\" <liga|desliga|pisca> [hz] [seg] | sched del <id> | "
                 "sched clear",
         .func = &cmd_sched},
        {.command = "status", .help = "lista os agendamentos e a hora atual", .func = &cmd_status},
        {.command = "time",
         .help = "time get | time set \"AAAA-MM-DD HH:MM:SS\" | time sync",
         .func = &cmd_time},
        {.command = "wifi",
         .help = "wifi <ssid> [senha] | wifi status | wifi forget",
         .func = &cmd_wifi},
        {.command = "led",
         .help = "aciona o LED direto: led <liga|desliga|pisca> [hz] [seg]",
         .func = &cmd_led},
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

    err = esp_console_register_help_command();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "registro do help falhou: %s", esp_err_to_name(err));
        return err;
    }

    return esp_console_start_repl(repl);
}
