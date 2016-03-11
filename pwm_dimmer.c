#include "pwm_dimmer.h"


void pwm_init(void)
{
	duty = 0;
    memset(pwm_en, 0, 1);

    PRINTF("\nStarting the PWM testing\n");

}

/*---------------------------------------------------------------------------*/
