# esp32-cron-scheduler

[![CI](https://github.com/DreamkitteXz/esp32-cron-scheduler/actions/workflows/ci.yml/badge.svg)](https://github.com/DreamkitteXz/esp32-cron-scheduler/actions/workflows/ci.yml)

Agendador de comandos por expressões CRON em ESP32 (ESP-IDF + FreeRTOS), controlando o LED
onboard (GPIO2) e configurável por uma CLI serial. Relógio por NTP ou ajuste manual, timezone
`America/Sao_Paulo`.

> **Estado atual: scaffold.** A estrutura, o build system, a CI e os contratos (headers) estão
> prontos e verdes. A lógica de negócio está marcada com `TODO(dev)` nos `.c` — ver
> [Roteiro de implementação](#roteiro-de-implementação).

## Visão geral

| Componente | Responsabilidade | Depende de |
|---|---|---|
| `cron` | Parse da expressão em bitmasks e match O(1). **C puro** — sem ESP-IDF/FreeRTOS. | — |
| `sched` | Tabela de 10 slots, mutex, software timer de 1 Hz, task e fila de comandos. | `cron`, `led` |
| `led` | Task consumidora de fila: `ON` / `OFF` / `BLINK` como máquina de estados. | — |
| `clk` | Wall clock, timezone, SNTP e event group de sincronismo. | `sched` |
| `cli` | `esp_console`: registro dos comandos, parsing e mensagens de erro. | `sched`, `clk`, `led` |
| `common` | Só o `sched_action_t`, compartilhado por `sched` e `led`. | — |

O `common` existe para quebrar um ciclo: `led.h` precisa do tipo da ação na assinatura de
`led_cmd()`, e `sched.c` precisa chamar `led_cmd()`. Com o enum em `sched.h`, `led → sched` e
`sched → led` fechariam um ciclo de `REQUIRES` no ESP-IDF.

## Build & Flash

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORTA> flash monitor
```

## Referência da CLI

| Comando | Descrição |
|---|---|
| `add "<expr cron>" <on\|off\|blink>` | Cria um agendamento e imprime o `id`. |
| `del <id>` | Remove o agendamento `id`. |
| `list` | Lista os agendamentos ativos. |
| `clear` | Remove todos os agendamentos. |
| `time now` | Mostra a hora local e se o relógio está sincronizado. |
| `time set "AAAA-MM-DD HH:MM:SS"` | Acerta o relógio manualmente. |
| `time sync` | Sincroniza por NTP. |
| `led <on\|off\|blink>` | Aciona o LED direto, sem passar pelo scheduler. |

## Formato CRON

Seis campos separados por espaço: `segundo minuto hora dia mês dia-da-semana`.

| Campo | Range | Exemplo |
|---|---|---|
| segundo | 0–59 | `*/15` |
| minuto | 0–59 | `30` |
| hora | 0–23 | `8,12,18` |
| dia do mês | 1–31 | `1` |
| mês | 1–12 | `1-6` |
| dia da semana | 0–6 (0 = domingo) | `1-5` |

Cada campo aceita `*`, valor único, lista (`a,b`), intervalo (`a-b`) e passo (`*/n`, `a-b/n`).

> **Premissa registrada:** o PDF do enunciado traz a tabela de ranges corrompida (`0–5U`).
> Assumimos `0–59` para segundo e minuto, e IDs de agendamento de `0–9`.

## Decisões de projeto

### Divisão de tasks

Três tasks, prioridade e stack fixadas como macros no topo de cada `.c`:

| Task | Prioridade | Stack | Por quê |
|---|---|---|---|
| `led` | 6 | 2048 | A mais alta: o pisca tem prazo (meio período). Se o scheduler a preemptasse, o duty cycle tremeria visivelmente. |
| `sched` | 5 | 3072 | Precisa acordar todo segundo e avaliar a tabela antes do próximo tick. |
| `cli` | 3 | 4096 | A mais baixa: ninguém morre se o eco do console atrasar 5 ms. Stack maior porque `esp_console` + `argtable` são gulosos. |

O callback do software timer de 1 Hz roda na Timer Service task e **só faz `xTaskNotifyGive`**.
Nunca pega mutex, nunca imprime, nunca bloqueia — segurar a Timer Service task atrasaria todos os
outros timers do sistema.

### Ausência de condição de corrida

A tabela tem **uma única escritora**: a task do `sched`. A CLI nunca a muta; ela enfileira um
`sched_cmd_t`, acorda a task e espera a resposta com timeout. O mutex protege só o `snapshot`
(copy-out) contra a escrita da task — e o chamador nunca recebe um ponteiro para dentro da tabela.

A seção crítica é curta de propósito: sob o mutex, apenas comparar com `cron_match()` e copiar as
ações a disparar para um vetor local. O mutex é solto **antes** de enfileirar para a task do LED —
nada de I/O nem de chamada bloqueante segurando o lock.

### Tratamento de salto de relógio

Um `time set` para o futuro (ou o primeiro sync NTP, que salta de 1970 para hoje) criaria um buraco
de milhões de segundos. Varrê-lo segundo a segundo dispararia uma avalanche de ações de uma vez.
Por isso `clk` chama `sched_notify_clock_jump()` após qualquer ajuste: se `now - last_eval > 5s`, o
scheduler faz **rebaseline** (`last_eval = now`) e simplesmente ignora o intervalo pulado.

### Parâmetros do pisca

`sched_add()` não recebe frequência nem duração — um agendamento `blink` usa os padrões do
componente (`LED_BLINK_DEFAULT_HZ = 2`, `LED_BLINK_DEFAULT_MS = 3000`). `led_cmd()` aceita os dois
como parâmetro (0 = usar o padrão), então dá para expor isso na CLI depois sem mexer no scheduler.

A task do LED usa `xQueueReceive` com timeout igual ao próximo toggle: ou chega comando novo, ou
vence o timeout e a máquina de estados avança um passo. É isso que faz um comando novo **preemptar**
um pisca em andamento, sem `vTaskDelay` bloqueante e sem timer extra.

### O que faria diferente com mais tempo

- Persistir a tabela em NVS, para os agendamentos sobreviverem a um reboot.
- Usar `cron_next_run()` para dormir até o próximo disparo em vez de acordar a 1 Hz — menos wakeups,
  melhor para consumo. O tick de 1 Hz foi escolhido por ser mais simples de auditar.
- Uma fila de resposta por chamador (hoje há uma só, assumindo um único cliente da CLI).

## Testes

`components/cron` é C puro justamente para poder ser testado no host, sem hardware e sem ESP-IDF:

```bash
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Os testes da lógica ainda não implementada estão marcados com `TEST_IGNORE_MESSAGE`, então a suíte
fica verde sem fingir que o parser funciona. Cada `IGNORE` removido é um `TODO(dev)` a menos.

## Roteiro de implementação

Ordem sugerida — cada passo deixa a suíte um pouco mais verde:

1. `cron_parse()` — campos, `*`, valor único, lista, intervalo, passo, validação de range.
2. `cron_match()` — AND dos 6 campos, com a regra OR entre dia-do-mês e dia-da-semana.
3. `sched` — a transação CLI→task (`add`/`del`/`clear`/`snapshot`) e a avaliação a cada tick.
4. `led` — a máquina de estados do pisca no `led_task()`.
5. `cli` — parsing dos argumentos e mensagens de erro dos comandos.
6. `clk` — `time set` (`settimeofday` + `sched_notify_clock_jump`) e o SNTP.
7. `sched_notify_clock_jump()` — o rebaseline.
8. `cron_next_run()` — bônus; habilita mostrar o próximo disparo no `list`.

## Limitações

- Máximo de 10 agendamentos simultâneos (`SCHED_MAX_ENTRIES`).
- Resolução de 1 s; um agendamento perdido durante um salto de relógio não é recuperado (por
  design, ver acima).
- Agendamentos vivem só em RAM: um reboot limpa a tabela.
- `clk_ntp_sync()` pressupõe uma rede já configurada; o provisionamento de Wi-Fi não faz parte
  deste escopo.
