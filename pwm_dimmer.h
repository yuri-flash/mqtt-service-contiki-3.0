#ifndef PWM_DIMMMER
#define PWM_DIMMMER

#include "contiki.h"
#include "cpu.h"
#include "dev/leds.h"
#include "dev/watchdog.h"
#include "dev/sys-ctrl.h"
#include "pwm.h"
#include "systick.h"
#include "lpm.h"
#include "dev/ioc.h"
#include <stdio.h>
#include <stdint.h>
#include "sys/etimer.h"
#include "sys/ctimer.h"

/*---------------------------------------------------------------------------*/
//#define DEBUG 1
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif
/*---------------------------------------------------------------------------*/
typedef struct {
  uint8_t timer;
  uint8_t ab;
  uint8_t port;
  uint8_t pin;
  uint8_t duty;
  uint8_t off_state;
  uint32_t freq;
} pwm_config_t;



/*---------------------------------------------------------------------------*/
static const pwm_config_t pwm_num = {
    .timer = PWM_TIMER_1,
    .ab = PWM_TIMER_B,
    .port = GPIO_D_NUM,
    .pin = 4,
    .duty = 75,
    .freq = 20000,
    .off_state = PWM_ON_WHEN_STOP,
  };
static uint8_t pwm_en;
/*---------------------------------------------------------------------------*/
#if DEBUG
static const char *
gpt_name(uint8_t timer)
{
  switch(timer) {
  case PWM_TIMER_0:
    return "PWM TIMER 0";
  case PWM_TIMER_1:
    return "PWM TIMER 1";
  case PWM_TIMER_2:
    return "PWM TIMER 2";
  case PWM_TIMER_3:
    return "PWM TIMER 3";
  default:
    return "Unknown";
  }
}
#endif
/*---------------------------------------------------------------------------*/
static struct etimer et_pwm;
/*---------------------------------------------------------------------------*/
uint8_t duty;


void pwm_init(void);

#endif /* PWM_DIMMMER */
