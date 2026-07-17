# esp32-cron-scheduler

[![CI](https://github.com/DreamkitteXz/esp32-cron-scheduler/actions/workflows/ci.yml/badge.svg)](https://github.com/DreamkitteXz/esp32-cron-scheduler/actions/workflows/ci.yml)

Agendador de comandos por expressões CRON em ESP32 (ESP-IDF + FreeRTOS): você descreve *quando*
algo deve acontecer com uma expressão de 6 campos, e o firmware aciona o LED onboard na hora certa.
Configurável por uma CLI serial, com relógio ajustado à mão ou por NTP..

## Visão geral

Até **10 agendamentos** simultâneos, cada um com uma expressão CRON e uma ação (`liga`, `desliga`
ou `pisca`). Um timer de 1 Hz avalia a tabela a cada segundo; o que casar vira comando para o LED.
O relógio usa o fuso `America/Sao_Paulo` (UTC-3), acertado por `time set` ou por NTP.

| Componente | Responsabilidade |
|---|---|
| `cron` | Compila a expressão em bitmasks e faz o match. |
| `sched` | tabela de 10 slots: timer de 1 Hz, task própria, fila de comandos e mutex. |
| `led` | Task consumidora de fila; `liga`/`desliga`/`pisca` como máquina de estados sobre o GPIO2. |
| `clk` | fuso, ajuste manual, SNTP e o aviso de salto de relógio. |
| `net` | Wi-Fi em modo estação, credencial em NVS e reconexão automática no boot. |
| `cli` | `esp_console`: registro dos comandos, parsing dos argumentos e mensagens de erro. |
| `common` | Só o `sched_action_t`, compartilhado por `sched` e `led`. |

O `common` existe para quebrar um ciclo de dependência: `led.h` precisa do tipo da ação na
assinatura de `led_cmd()`, e `sched.c` precisa chamar `led_cmd()`. Com o enum em `sched.h`,
`led → sched` e `sched → led` fechariam um ciclo de `REQUIRES` no ESP-IDF.

## Uso de IA no desenvolvimento

Usei IA como par de programação ao longo de todo o projeto. Pedi para ela documentar todo o código com comentários para facilitar a leitura, além de pedir para ela subir os commits com mensagens fáceis. A implementação foi feita **por mim em
conjunto com a IA**: eu conduzi as decisões de arquitetura, defini o escopo de cada etapa,
revisei o que entrou e rejeitei o que não me convenceu. Trabalhamos em fases — parser, concorrência,
LED, CLI/relógio, Wi-Fi — e cada fase só avançava depois de eu entender o que tinha sido feito.
### Fluxo de concorrência

```
   Timer 1 Hz  (roda na Timer Service task)
        │
        │  xTaskNotifyGive() — SÓ notifica: sem mutex, sem printf, sem bloquear
        ▼
  ┌──────────────────────────┐     led_cmd()      ┌────────────────────────┐
  │  task sched   (prio 5)   │ ── fila, não ────▶ │  task led    (prio 6)  │ ──▶ GPIO2
  │  DONA ÚNICA da tabela    │    bloqueante      │  máquina do pisca      │
  └──────────────────────────┘                    └────────────────────────┘
        ▲              ▲
        │              │  sched_notify_clock_jump()  ── clk, após time set / NTP
        │              │
        │  fila de comandos  +  fila de resposta (com timeout)
        │
  ┌──────────────────────────┐
  │  task cli     (prio 3)   │   nunca muta a tabela; só enfileira e espera
  └──────────────────────────┘   (leitura: sched_snapshot, copy-out sob mutex)
```

## Build e Flash

**Pré-requisitos:** ESP-IDF **v5.3 ou superior** (a CI fixa a v5.3; o desenvolvimento também rodou
na v6.0.2) e uma placa ESP32 com LED onboard no GPIO2.

```bash
. $IDF_PATH/export.sh          # Linux/macOS;  no Windows: %IDF_PATH%\export.bat
idf.py set-target esp32
idf.py build
idf.py -p <PORTA> flash monitor
```

O binário fica em torno de **850 KB** — o stack Wi-Fi responde por cerca de dois terços disso.
A partição de app padrão tem 1 MB, então sobram ~19% de folga.

## Tutorial de uso

Sessão do zero no monitor serial. O prompt é `cron>`; as linhas `I (...)` são logs do firmware.

### 1. Acertar o relógio

O ESP32 não tem bateria de RTC: **ao ligar, ele acha que é 1970**. Nenhum agendamento faz sentido
antes de acertar a hora. Sem rede, use `time set`:

```
cron> time set "2026-07-16 08:29:55"
relogio ajustado
I (5210) clk: relogio ajustado manualmente para 2026-07-16 08:29:55
I (5215) sched: relogio saltou 1784201390 s: rebaseline, 1784201390 segundos ignorados
```

Aquele log de `rebaseline` é intencional: o relógio saltou 56 anos, e o scheduler **ignora** o
intervalo pulado em vez de avaliar cada um daqueles segundos (ver "Tratamento de salto de relógio").

A data é interpretada como **hora local de São Paulo** (UTC-3), não UTC:

```
cron> time get
2026-07-16 08:29:58 -03 (UTC-0300)
relogio sincronizado
```

Com Wi-Fi, dá para usar NTP em vez do ajuste manual:

```
cron> wifi MinhaRede minhasenha
conectando a 'MinhaRede'...
wifi: conectado em 'MinhaRede', ip 192.168.0.42
cron> time sync
sincronizando com NTP...
relogio sincronizado por NTP
```

A credencial fica salva em NVS: no próximo boot ele reconecta sozinho.

### 2. Criar os três tipos de agendamento

```
cron> sched add "0 30 8 * * 1-5" liga
agendamento 0 criado
```
> Segundo 0, minuto 30, hora 8, qualquer dia, qualquer mês, dias 1–5 (segunda a sexta).
> **Às 8:30:00 em dias úteis, o LED acende e fica aceso.**

```
cron> sched add "0 0 18 * * *" desliga
agendamento 1 criado
```
> **Todo dia às 18:00:00, o LED apaga.**

```
cron> sched add "*/10 * * * * *" pisca
agendamento 2 criado
```
> `*/10` no campo dos segundos = a cada 10 segundos (`:00`, `:10`, `:20`…).
> **O LED pisca a 5 Hz durante 3 s** (o padrão) e depois apaga.

O `pisca` aceita frequência e duração opcionais — `pisca <hz> <segundos>`:

```
cron> sched add "0 * * * * *" pisca 10 2
agendamento 3 criado
```
> **No segundo :00 de cada minuto, pisca a 10 Hz por 2 s.**

Para testar o LED na hora, sem esperar o agendamento, use o comando `led`:

```
cron> led pisca 2 5
led: pisca
```
> Pisca a 2 Hz por 5 s, imediatamente. Um comando novo **interrompe** o pisca em andamento —
> `led liga` no meio dele acende na hora, sem esperar os 5 s acabarem.

### 3. Ver o estado

```
cron> status
hora: 2026-07-16 08:30:05 -03 (relogio sincronizado)
rede: desconectado

ID  EXPRESSAO                ACAO     PISCA    ULTIMO DISPARO
0   0 30 8 * * 1-5           liga     -        2026-07-16 08:30:00
1   0 0 18 * * *             desliga  -        nunca
2   */10 * * * * *           pisca    5Hz/3s   2026-07-16 08:30:00
3   0 * * * * *              pisca    10Hz/2s  2026-07-16 08:30:00

4/10 slots em uso
```

`ULTIMO DISPARO` mostra `nunca` até o agendamento casar pela primeira vez.

### 4. Remover

```
cron> sched del 1
agendamento 1 removido

cron> sched clear
3 agendamento(s) removido(s)
```

### 5. O que acontece com entrada inválida

Cada erro que o firmware sabe distinguir tem mensagem própria:

```
cron> sched add "60 * * * * *" liga
erro: expressao cron invalida: valor fora do intervalo permitido para o campo
      formato: <seg> <min> <hora> <dia> <mes> <dia-semana>
```
> Segundo vai só até 59.

```
cron> sched add "* * * * *" liga
erro: expressao cron invalida: expressao deve ter 6 campos: seg min hora dia mes dia-da-semana
      formato: <seg> <min> <hora> <dia> <mes> <dia-semana>
```
> Cinco campos — é o CRON clássico do Unix, que não tem segundos. Aqui são 6.

```
cron> sched add "* * * * * *" acender
erro: acao invalida: 'acender' (use liga, desliga ou pisca)

cron> sched del 7
erro: agendamento 7 nao existe

cron> sched add "* * * * * *" liga      (na 11ª vez)
erro: limite de 10 agendamentos atingido (use 'sched del' ou 'sched clear')

cron> time set "2026-02-31 10:00:00"
erro: data/hora invalida: '2026-02-31 10:00:00'
      formato: "AAAA-MM-DD HH:MM:SS" (ex.: "2026-07-16 08:30:00")
```
> 31 de fevereiro não existe. Sem essa checagem, a libc normalizaria para 03/03 em silêncio e o
> relógio iria para o dia errado.

```
cron> time sync                          (sem Wi-Fi)
erro: sem rede. Use 'wifi <ssid> <senha>' antes de sincronizar,
      ou acerte o relogio na mao com 'time set'.
```

## Referência da CLI

| Comando | Argumentos | Descrição |
|---|---|---|
| `help` | — | Lista todos os comandos. |
| `sched add` | `"<expr cron>" <liga\|desliga\|pisca> [hz] [seg]` | Cria um agendamento; imprime o id (0–9). `hz`/`seg` só valem para `pisca`. |
| `sched del` | `<id>` | Remove o agendamento `id`. |
| `sched clear` | — | Remove todos e informa quantos foram removidos. |
| `status` | — | Hora, estado do relógio, rede e a tabela de agendamentos. |
| `time get` | — | Hora local, fuso e se o relógio está acertado. |
| `time set` | `"AAAA-MM-DD HH:MM:SS"` | Acerta o relógio à mão. Funciona **sem rede**. |
| `time sync` | — | Sincroniza por NTP (exige Wi-Fi conectado). |
| `wifi` | `<ssid> [senha]` | Conecta e salva a credencial em NVS. Sem senha = rede aberta. |
| `wifi status` | — | Estado da conexão e IP. |
| `wifi forget` | — | Apaga a credencial salva. |
| `led` | `<liga\|desliga\|pisca> [hz] [seg]` | Aciona o LED na hora, sem passar pelo agendador. |

## Formato CRON

Uma expressão CRON descreve **um conjunto de instantes**, não um horário único. Aqui são **6 campos
separados por espaço** — o CRON clássico do Unix tem 5; o sexto (o primeiro, aqui) é o segundo:

```
   ┌───────────── segundo        (0–59)
   │ ┌─────────── minuto         (0–59)
   │ │ ┌───────── hora           (0–23)
   │ │ │ ┌─────── dia do mês     (1–31)
   │ │ │ │ ┌───── mês            (1–12)
   │ │ │ │ │ ┌─── dia da semana  (0–6, 0 = domingo)
   │ │ │ │ │ │
   0 30 8 * * 1-5
```

Um instante casa quando **todos** os campos casam.

| Campo | Range |
|---|---|
| segundo | 0–59 |
| minuto | 0–59 |
| hora | 0–23 |
| dia do mês | 1–31 |
| mês | 1–12 |
| dia da semana | 0–6 (0 = domingo) |

### O que cada campo aceita

| Sintaxe | Significado | Exemplo lido em português |
|---|---|---|
| `*` | qualquer valor | `* * * * * *` = **todo segundo** |
| `N` | valor exato | `0 0 12 * * *` = **todo dia ao meio-dia em ponto** |
| `a,b,c` | lista | `0 0 8,12,18 * * *` = **às 8h, 12h e 18h** |
| `a-b` | intervalo | `0 0 9 * * 1-5` = **às 9h de segunda a sexta** |
| `*/n` | passo | `*/15 * * * * *` = **a cada 15 segundos** (:00, :15, :30, :45) |
| `a-b/n` | passo dentro do intervalo | `0 0 8-18/2 * * *` = **de 2 em 2 horas, das 8h às 18h** |

Mais exemplos:

- `0 30 8 * * 1-5` — 8:30:00, de segunda a sexta.
- `0 0 0 1 * *` — meia-noite do dia 1º de todo mês.
- `*/10 * * * * *` — a cada 10 segundos, sempre.
- `0 */5 * * * *` — a cada 5 minutos (no segundo 0).

### A regra dia-do-mês × dia-da-semana

Utilizei a semântica do **Vixie cron** (o cron padrão do Linux): quando **os dois** campos são
restritos, a combinação é **OU**, não E.

```
0 0 0 1 * 1     →  meia-noite do dia 1º  OU  de toda segunda-feira
```

Parece inconsistente com o resto (onde tudo é E), e é mesmo — mas é o comportamento que 40 anos de
crontabs assumem, e mudá-lo surpreenderia mais do que ajudaria.

um detalhe: **"restrito" é sintático, não semântico** — o campo não ser literalmente `*`. Isso
importa porque `1-31` acende exatamente os mesmos bits que `*`, mas conta como restrito:

```
0 0 0 1-31 * 1  →  dispara TODO DIA        (regra OU: "qualquer dia" ou "segunda")
0 0 0 *    * 1  →  dispara só nas segundas (regra E: dia-do-mês é "*", então não restringe)
```

As duas expressões descrevem o mesmo conjunto de dias-do-mês e mesmo assim se comportam diferente.
Por isso o parser guarda se o campo era literalmente `*`, e não só a bitmask.

## Como funciona a CI (GitHub Actions)

O arquivo [`.github/workflows/ci.yml`](.github/workflows/ci.yml) descreve o que o GitHub roda
sozinho, em máquinas dele, a cada `push` na `main` e a cada pull request (também dá para disparar à
mão pela aba *Actions*). A ideia: nada entra no repositório sem compilar e passar nos testes, e
isso não depende de ninguém lembrar de rodar nada.

São três **jobs**, e a ordem não é acidental — é **fail-fast**, do mais barato para o mais caro:

**1. `lint` (~30 s).** Roda `clang-format --dry-run --Werror` (formatação) e `cppcheck` (análise
estática) sobre o código. Falha em segundos se algo está torto, antes de gastar minutos compilando.
O `clang-format` vem do `pip` com **versão fixa** (`20.1.8`) em vez do `apt`: a versão do `apt`
acompanha a imagem do runner, e versões diferentes discordam da largura de caracteres UTF-8 e
realinham comentários sozinhas — o lint quebraria sozinho num bump da imagem. *(Isso aconteceu de
verdade aqui, e foi assim que descobrimos.)*

**2. `host-tests` (~40 s).** Compila e roda os testes do parser CRON **no Ubuntu, sem ESP32**. Só é
possível porque `components/cron` é C puro; é o CMake de `tests/host` que o compila junto com o
Unity. Testar no PC é mais rápido que gravar num chip, e roda em cada push.

**3. `build` (~5 min).** O firmware compila para ESP32. Usa a action oficial
`espressif/esp-idf-ci-action@v1`, que roda o build **dentro do container Docker oficial da
Espressif** com o ESP-IDF já instalado. É melhor que instalar o IDF na mão no runner porque o IDF
tem toolchain, Python e dezenas de dependências: a instalação manual seria lenta, frágil e
divergiria do ambiente que a Espressif testa. A versão é fixada em `v5.3` — nada de `latest`, que
transforma "a CI quebrou" num mistério sem ninguém ter mudado o código. Ao fim, sobe o `.bin` e o
`.elf` como **artifact**, baixável pela página da run.

Dois detalhes do YAML:

- **`needs: [lint, host-tests]`** no job de build: ele só começa se os dois passarem. Sem isso, o
  GitHub rodaria os três em paralelo e gastaria 5 minutos de build para depois avisar que faltava
  um espaço no código.
- **`concurrency` com `cancel-in-progress: true`**: se você empurrar dois commits seguidos, a run
  antiga é cancelada. Economiza minutos e, principalmente, evita um "verde" que se refere a um
  commit que já não é o topo do branch.

## Testes

A estratégia é dividir o que precisa de hardware do que não precisa. `components/cron` — o parser e
o match — **não inclui nenhum header do ESP-IDF, do FreeRTOS ou de
driver**. Essa restrição é o que permite compilá-lo num PC e testá-lo com Unity + CTest, sem gravar
nada num chip:

```bash
cmake -S tests/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

São **21 testes**, cobrindo:

- **Parse**: curingas, valor exato, listas, intervalos, passos.
- **Casos de borda**: contagem de campos ≠ 6, valor fora do range, step 0 (`*/0`), intervalo
  invertido (`5-1`), token vazio (`1,,3`, `,5`, `5,`, `1-`, `-5`), lixo não-numérico (`1a`).
- **Match**: AND dos campos, a regra OU do Vixie (inclusive o caso `1-31` vs `*`), e o off-by-one
  entre `tm_mon` (0–11) e a máscara (1–12).
- **`cron_next_run`**: virada de dia, mês e ano, 29 de fevereiro, e uma expressão impossível
  (`30/02`) para garantir que a busca termina em vez de girar para sempre.

Rodam automaticamente no job `host-tests` da CI, a cada push.

### O que os testes de host não alcançam

Parser é uma função pura: fácil de testar, e por isso testado a fundo. O resto do firmware —
concorrência, timing do pisca, GPIO — não cabe num teste de host, e foi **validado à mão numa
ESP32 DevKit**: boot e CLI respondendo, os três tipos de agendamento disparando no LED, a
interrupção do pisca por um comando novo, o rebaseline num `time set` com salto grande (sem
avalanche de disparos), e o Wi-Fi com `time sync` sincronizando de verdade.

> Se for reformatar o código, use **`clang-format==20.1.8`** (`pip install clang-format==20.1.8`) —
> a mesma versão que a CI fixa.

## Decisões de projeto

### Divisão de tasks

| Task | Prioridade | Stack | Por quê |
|---|---|---|---|
| `led` | 6 | 2048 | Ao pisca tem prazo (meio período de 100 ms) |
| `sched` | 5 | 3072 | Precisa acordar todo segundo e avaliar a tabela antes do próximo tick. |
| `cli` | 3 | 4096 | ninguém morre se o eco do console atrasar alguns ms.|

Valores como macros no topo de cada `.c`. Além dessas rodam a Timer Service task (o tick de 1 Hz) e
a task do event loop do IDF (eventos de Wi-Fi/IP).

Em `app_main`, **o retorno de toda criação de objeto RTOS é checado** — fila, task, mutex, timer.
Se algum não nascer, o boot aborta com log dizendo qual módulo falhou.

### Tratamento de salto de relógio

O tick de 1 Hz conta ticks do RTOS — os dois derivam entre si, e
um tick que escorregue pularia um segundo inteiro. Por isso a avaliação varre a **faixa**
`(last_eval, now]`, não só `now`.

Essa varredura precisa de teto. Um `time set` para o futuro, ou o primeiro sync NTP (que salta de
1970 para hoje), abre um buraco de ~1,7 bilhão de segundos — varrê-lo dispararia uma avalanche de
ações de uma vez. Então: se `now - last_eval > 5 s`, o scheduler faz **rebaseline**
(`last_eval = now`) e ignora o intervalo pulado. Abaixo de 5 s, varre normalmente — um ajuste fino
de relógio não perde agendamento. O teto é uma escolha, não um "ignore tudo".

`clk` chama `sched_notify_clock_jump()` após qualquer ajuste. Repare no que essa função **não**
faz: não toca em `last_eval` nem levanta flag compartilhada. A decisão mora na task dona do estado;
a notificação é só "acorde e olhe o relógio agora", o que aplica o efeito no instante do ajuste em
vez de até 1 s depois.

**Onde a IA mais ajudou foi justamente nos assuntos que eu não dominava:**

- **GitHub Actions.** Eu nunca tinha configurado um workflow. Usei a IA para entender o modelo —
  jobs, `needs`, `concurrency`, por que usar o container oficial da Espressif em vez de instalar o
  IDF no runner, por que fixar a versão em vez de usar `latest`. A seção "Como funciona a CI" acima
  é o resultado desse aprendizado, escrita para o próximo que chegar sem saber o que é um runner.
- **A semântica fina do CRON.** Eu não conhecia o CRON. A regra dia-do-mês ×
  dia-da-semana do Vixie — e principalmente de que "restrito" é *sintático*, então
  `1-31` não é a mesma coisa que `*` — foi algo que eu só entendi discutindo o problema. Virou
  teste, comentário no código e uma seção do README.
- **Boas práticas de concorrência em FreeRTOS.** Por que o callback de um software timer não pode
  bloquear (ele roda na Timer Service task e atrasaria todos os outros timers); por que uma tabela
  com dono único é mais fácil de defender que uma tabela cheia de locks; por que o comando tem que
  ir na fila por valor e não por ponteiro quando existe timeout do lado do chamador.

**O que ficou comigo, além do código:** entendo o fluxo de concorrência a ponto de desenhá-lo do
zero — quem acorda quem, o que cada mutex protege, e o que aconteceria se cada regra fosse violada.
O mesmo vale para o workflow de CI e para o parser. As seções "Decisões de projeto" e "Formato CRON"
deste README são a evidência: elas não descrevem o que o código faz — descrevem **por que** ele faz
assim, e o que a alternativa custaria.

Revisei e entendo cada parte do que está aqui, e estou à disposição para conversar sobre qualquer
decisão do código.

## Limitações conhecidas

- **A validação no alvo foi manual, não automatizada.** O comportamento na placa foi conferido à
  mão; 
- Máximo de 10 agendamentos simultâneos.
- Agendamentos vivem só em RAM: reboot limpa a tabela.
- Resolução de 1 s. Agendamento perdido num salto de relógio > 5 s não é recuperado (por design).
- **A sincronização NTP chama-se `time sync`**, agrupada com os demais subcomandos de relógio.
- `cron_next_run()` está implementado e testado, mas **não é exposto na CLI** — o `status` mostra o
  último disparo, não o próximo.
- **Partição de app com ~19% livre.** O stack Wi-Fi custa ~565 KB e o binário está em ~850 KB de
  1 MB. Crescer muito, ou querer OTA, exige uma `partitions.csv` customizada.
- Credencial do Wi-Fi gravada em NVS sem criptografia.
- O provisionamento de Wi-Fi é por CLI (`wifi <ssid> <senha>`); não há portal cativo nem SmartConfig.
