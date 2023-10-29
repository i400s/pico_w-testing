#include <stdio.h>
#include "src/msp2807.h"
#include "hardware/pwm.h"

static volatile int backlight_brightness = BACKLIGHT_MAX;
static volatile alarm_id_t alarm_id = 0;
static volatile alarm_callback_t alarm_callback = NULL;

void backlight_check_timer(alarm_id_t id) {
    // if this timer firing is the one set by the touchscreen event
    // then the timer is complete so it can be cleared.
    if (id == alarm_id) {
        printf("Alarm complete for id: %d\n", alarm_id);
        alarm_id = 0;
    }
}

void msp2807_reset_irq(void) {
    backlight_brightness = BACKLIGHT_MAX;
    pwm_set_gpio_level(BACKLIGHT_LED, (backlight_brightness * backlight_brightness));

    // If active alarm then cancel it
    if (alarm_id > 0) {
        cancel_alarm(alarm_id);
        printf("Cancel: %d ", alarm_id);
    }
    // Call alarm_callback in 60 seconds
    alarm_id = add_alarm_in_ms(60000, alarm_callback, NULL, false);
    printf("Set: %d ", alarm_id);
}

void touchscreen_init(gpio_irq_callback_t callback) {
    gpio_set_irq_enabled(TOUCHSCREEN_IRQ, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_pull_up(TOUCHSCREEN_IRQ);
}


void backlight_pwm_wrap(void) {
    static volatile int null_loop = 0;

    pwm_clear_irq(pwm_gpio_to_slice_num(BACKLIGHT_LED));

    if (alarm_id != 0) {
        return;
    }

    // As we change the brightness in only 255 steps we need to
    // lengthen the time it takes to perform the stepped fade.
    if (null_loop <= BACKLIGHT_STEP) {
        null_loop += 1;
        return;
    } else {
        null_loop = 0;
    }

    pwm_set_gpio_level(BACKLIGHT_LED, (backlight_brightness * backlight_brightness));
    if (backlight_brightness == 0) {
        return;
    }

    // printf("brightness: %i\n", brightness);
    backlight_brightness -= 1;
    if (backlight_brightness <= 0) {
        backlight_brightness = 0;
    }
}


void backlight_init(alarm_callback_t callback) {
    alarm_callback = callback;
    gpio_set_function(BACKLIGHT_LED, GPIO_FUNC_PWM);
    uint backlight_slice = pwm_gpio_to_slice_num(BACKLIGHT_LED);
    pwm_clear_irq(backlight_slice);
    pwm_set_irq_enabled(backlight_slice, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, backlight_pwm_wrap);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_config backlight_config = pwm_get_default_config();
    pwm_config_set_clkdiv(&backlight_config, 4.f);
    pwm_init(backlight_slice, &backlight_config, true);
}


