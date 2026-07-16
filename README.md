# esp32-cron-scheduler

[![CI](https://github.com/DreamkitteXz/esp32-cron-scheduler/actions/workflows/ci.yml/badge.svg)](https://github.com/DreamkitteXz/esp32-cron-scheduler/actions/workflows/ci.yml)

Agendador de comandos por expressões CRON em ESP32 (ESP-IDF + FreeRTOS), controlando o LED
onboard (GPIO2) e configurável por uma CLI serial. Até 10 agendamentos, relógio por NTP ou ajuste
manual, timezone `America/Sao_Paulo` (UTC-3).

## Visão geral

| Componente | Responsabilidade | Depende de |
|---|---|---|
| `cron` | Parse da expressão em bitmasks e match O(1). **C puro** — sem ESP-IDF/FreeRTOS. | — |
| `sched` | Tabela de 10 slots, task dona, mutex, software timer de 1 Hz, fila de comandos. | `cron`, `led` |
| `led` | Task consumidora de fila: liga / desliga / pisca como máquina de estados. | `common` |
| `clk` | Wall clock, timezone, SNTP e event group de sincronismo. | `sched` |
| `net` | Wi-Fi em modo STA, credenciais em NVS, reconexão no boot. | — |
| `cli` | `esp_console`: registro dos comandos, parsing e mensagens de erro. | `sched`, `clk`, `led`, `net` |
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

Testado com ESP-IDF **v6.0.2** (local) e **v5.3** (CI).

## Referência da CLI

| Comando | Descrição |
|---|---|
| `help` | Lista os comandos. |
| `sched add "<expr cron>" <liga\|desliga\|pisca> [hz] [seg]` | Cria um agendamento e imprime o `id` (0–9). `hz`/`seg` só valem para `pisca`. |
| `sched del <id>` | Remove o agendamento `id`. |
| `sched clear` | Remove todos e informa quantos foram removidos. |
| `status` | Hora, estado do relógio, rede, e a tabela de agendamentos. |
| `time get` | Hora local + fuso + se o relógio está sincronizado. |
| `time set "AAAA-MM-DD HH:MM:SS"` | Acerta o relógio manualmente. Funciona **sem rede**. |
| `time sync` | Sincroniza por NTP (equivale ao `ntp sync` do enunciado — ver Limitações). |
| `wifi <ssid> [senha]` | Conecta e salva a credencial em NVS. Sem senha = rede aberta. |
| `wifi status` / `wifi forget` | Mostra estado/IP; apaga a credencial. |
| `led <liga\|desliga\|pisca> [hz] [seg]` | Aciona o LED direto, sem passar pelo scheduler. |

Exemplo:

```
cron> time set "2026-07-16 08:29:55"
relogio ajustado
cron> sched add "0 30 8 * * 1-5" liga
agendamento 0 criado
cron> sched add "*/10 * * * * *" pisca 10 2
agendamento 1 criado
cron> status
hora: 2026-07-16 08:29:58 -03 (relogio sincronizado)
rede: desconectado

ID  EXPRESSAO                ACAO     PISCA    ULTIMO DISPARO
0   0 30 8 * * 1-5           liga     -        nunca
1   */10 * * * * *           pisca    10Hz/2s  2026-07-16 08:29:50

2/10 slots em uso
cron> sched del 0
agendamento 0 removido
```

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

Cada item aceita `*`, valor único (`N`), lista (`a,b`), intervalo (`a-b`) e passo (`*/n`, `a-b/n`).

## Premissas assumidas

Registradas porque são escolhas, não fatos do enunciado:

1. **Tabela de ranges corrompida no PDF** (`0–5U`). Assumimos **0–59** para segundo e minuto, e
   **IDs de agendamento 0–9**.
2. **Dia-do-mês × dia-da-semana: semântica Vixie cron** — quando **ambos** são restritos, a
   combinação é **OR**, não AND. Ver "Decisões de projeto".
3. **Pisca**: default **5 Hz** (100 ms aceso / 100 ms apagado) por **3 s**. Faixa aceita: 1–50 Hz.
4. **`N/S` não é gramática válida** (só `*/S` e `N-M/S`). O Vixie real aceitaria `5/2` como
   `5-59/2`; aqui é erro de sintaxe, seguindo a gramática do enunciado.
5. **Intervalo invertido** (`5-1`) é `CRON_ERR_RANGE`, sem wrap-around.

## Decisões de projeto

### Divisão de tasks

| Task | Prioridade | Stack | Por quê |
|---|---|---|---|
| `led` | 6 | 2048 | A mais alta: o pisca tem prazo (meio período). Se o scheduler a preemptasse, o duty cycle tremeria visivelmente. |
| `sched` | 5 | 3072 | Precisa acordar todo segundo e avaliar a tabela antes do próximo tick. |
| `cli` | 3 | 4096 | A mais baixa: ninguém morre se o eco do console atrasar 5 ms. Stack maior porque `esp_console` é guloso. |

Além dessas, rodam a Timer Service task (o tick de 1 Hz) e a task do event loop do IDF (eventos de
Wi-Fi/IP). Prioridades e stacks estão como macros no topo de cada `.c`.

### Ausência de condição de corrida

Três mecanismos, cada um resolvendo um problema diferente:

1. **Dono único.** A tabela tem uma só escritora: a task do `sched`. A CLI **nunca** a muta —
   monta um `sched_cmd_t`, enfileira, acorda a task e espera a resposta com timeout. Isso torna
   "achar slot livre + ocupá-lo" atômico por construção, sem TOCTOU, e sem a CLI segurar lock.
2. **Mutex para o copy-out.** O `s_table_mutex` existe por **uma** razão: `sched_snapshot` lê a
   tabela no contexto do *chamador*, então precisa se proteger contra a escrita da task. O chamador
   recebe uma cópia, nunca um ponteiro para dentro da tabela.
3. **`s_api_mutex`.** Serializa os chamadores da API, o que torna seguro haver uma única fila de
   resposta compartilhada — senão a resposta de um chamador poderia ser lida por outro.

A seção crítica é curta de propósito: sob o mutex, apenas comparar com `cron_match()` e copiar as
ações a disparar para um vetor local. O mutex é solto **antes** de chamar `led_cmd()` — nada de I/O
nem de chamada bloqueante segurando o lock.

O callback do software timer de 1 Hz roda na Timer Service task e **só faz `xTaskNotifyGive`**.
Nunca pega mutex, nunca imprime, nunca bloqueia — segurar aquela task atrasaria todos os outros
timers do sistema.

O comando vai para a fila **por valor**, não por ponteiro: se o chamador estoura o timeout e
retorna, seu buffer na pilha morre, mas a task ainda pode processar o comando depois. Um ponteiro
ali viraria *dangling*; a cópia elimina a classe do bug.

### Tratamento de salto de relógio

O tick de 1 Hz conta ticks do RTOS, não segundos do relógio de parede — os dois derivam, e um tick
que escorregue pularia um segundo inteiro. Por isso a avaliação varre a **faixa** `(last_eval, now]`,
não só `now`.

Essa varredura precisa de teto. Um `time set` para o futuro, ou o primeiro sync NTP (que salta de
1970 para hoje), criaria um buraco de ~1,7 bilhão de segundos e uma avalanche de disparos. Então:
se `now - last_eval > 5 s`, o scheduler faz **rebaseline** (`last_eval = now`) e ignora o intervalo
pulado. Abaixo de 5 s, varre normalmente — ajuste fino de relógio não perde agendamento.

`clk` chama `sched_notify_clock_jump()` após qualquer ajuste. Note o que essa função **não** faz:
não toca em `last_eval` nem levanta flag compartilhada. A decisão de rebaseline mora na task dona
do estado; a notificação é só "acorde e olhe o relógio agora", aplicando o efeito no instante do
ajuste em vez de até 1 s depois.

### Semântica dia-do-mês × dia-da-semana

Seguimos o **Vixie cron**: quando os dois campos são restritos, a combinação é **OR**.
`0 0 0 1 * 1` casa no dia 1 **ou** em toda segunda-feira.

O detalhe não óbvio: "restrito" ali é **sintático**, não semântico — o campo não ser literalmente
`*`. Isso é indeduzível da bitmask, porque `1-31` acende exatamente os mesmos bits que `*` mas
**conta como restrito**: `0 0 0 1-31 * 1` dispara todo dia (via OR), enquanto `0 0 0 * * 1` só nas
segundas. Por isso o `cron_expr_t` guarda `mday_star`/`wday_star`, preenchidos no parse. Coberto
por teste (`test_match_intervalo_cheio_conta_como_restrito`).

### Parâmetros do pisca

Default 5 Hz por 3 s; `sched add "..." pisca <hz> <seg>` e `led pisca <hz> <seg>` sobrepõem. A
faixa 1–50 Hz é validada **na CLI e no `sched_add`**, não só no `led_cmd`: aceitar um agendamento
que só falharia no disparo — horas depois e longe do comando que o criou — é pior que recusá-lo na
hora.

A task do LED usa `xQueueReceive` cujo **timeout é o instante do próximo toggle**. Ou chega comando
novo e o estado troca na hora, ou o timeout vence e a máquina dá um passo. É isso que faz um comando
novo **preemptar** o pisca em andamento, sem `vTaskDelay` bloqueante (que ignoraria a fila) e sem um
timer extra. A duração é contada para baixo em vez de comparada com `xTaskGetTickCount()`, porque o
contador de ticks estoura (~49 dias a 1 kHz) e a comparação de instantes quebraria na virada.

### O que faria diferente com mais tempo

- **Persistir a tabela em NVS.** Hoje um reboot limpa os agendamentos, mas a credencial do Wi-Fi
  sobrevive — é incoerente.
- **Dormir até o próximo disparo** usando `cron_next_run()` (já implementado e testado, hoje só
  bônus) em vez de acordar a 1 Hz. Menos wakeups, melhor consumo. O tick fixo foi escolhido por ser
  mais simples de auditar.
- **Uma fila de resposta por chamador**, em vez do `s_api_mutex` serializando todo mundo. Funciona
  com uma CLI; não escalaria para dois clientes.
- **Criptografar a credencial do Wi-Fi**: hoje ela vai em NVS em texto claro.

## Testes

`components/cron` é C puro justamente para poder ser testado no host, sem hardware e sem ESP-IDF:

```bash
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

21 testes cobrindo parse (curingas, listas, intervalos, passos, contagem de campos, ranges,
sintaxe, itens vazios), match (AND, a regra OR do Vixie, off-by-one do `tm_mon`) e `next_run`
(virada de dia/mês/ano, 29/02, expressão impossível).

A CI roda três jobs com fail-fast: `lint` (clang-format + cppcheck), `host-tests` e `build`
(ESP-IDF v5.3), com o `build` dependendo dos dois primeiros.

## Limitações

- **Nada foi validado em hardware.** O código compila e os testes de host passam; o comportamento
  no LED (pisca, preempção, rebaseline) foi verificado por leitura de código, não por execução.
- **Não existe comando `monitor`.** O `status` dá uma foto sob demanda, mas não há stream contínuo
  de disparos no console.
- **`ntp sync` chama-se `time sync`** aqui, agrupado com os demais subcomandos de relógio.
- Máximo de 10 agendamentos simultâneos (`SCHED_MAX_ENTRIES`).
- Resolução de 1 s. Agendamento perdido num salto de relógio > 5 s não é recuperado (por design).
- Agendamentos vivem só em RAM: reboot limpa a tabela.
- **Partição de app com 19% livre.** O stack Wi-Fi custa ~565 KB; o binário está em ~850 KB de
  1 MB. Crescer muito, ou querer OTA, exige uma `partitions.csv` customizada.
- Credencial do Wi-Fi em NVS sem criptografia.
- `time sync` exige rede associada (`wifi <ssid> <senha>`) — sem ela, falha na hora com aviso.
