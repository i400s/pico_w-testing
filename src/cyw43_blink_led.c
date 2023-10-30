#include "src/cyw43_blink_led.h"
static repeating_timer_t timer;

void cyw43_blink_led(alarm_id_t id) {
    static int blink_led_state = 0;
    if (id == timer.alarm_id) {
        if (blink_led_state == 0) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        } else {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        }
        blink_led_state = !blink_led_state;
    }
}

void cyw43_blink_led_init(repeating_timer_callback_t callback) {
    add_repeating_timer_ms(3000, callback, NULL, &timer);
}