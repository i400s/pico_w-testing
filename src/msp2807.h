#ifndef _msp2807_H
#define _msp2807_H

#include "pico/stdlib.h"

#define BACKLIGHT_LED 7
#define TOUCHSCREEN_IRQ 3
#define BACKLIGHT_MAX 0xFF
#define BACKLIGHT_STEP 35

void backlight_pwm_wrap(void);
void backlight_init(alarm_callback_t callback);
void backlight_check_timer(alarm_id_t id);
void touchscreen_init(gpio_irq_callback_t callback);
void msp2807_reset_irq(void);

#endif