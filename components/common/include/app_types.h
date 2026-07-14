#ifndef APP_TYPES_H_
#define APP_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ação disparada por um agendamento.
 *
 * Vive aqui, e não em sched.h, para quebrar um ciclo de dependência entre componentes:
 * led.h precisa do tipo na assinatura de led_cmd(), e sched.c precisa chamar led_cmd().
 * Se o enum morasse em sched.h, led -> sched e sched -> led fechariam um ciclo de REQUIRES.
 */
typedef enum {
    ACT_ON = 0,
    ACT_OFF,
    ACT_BLINK,
} sched_action_t;

#ifdef __cplusplus
}
#endif

#endif  // APP_TYPES_H_
