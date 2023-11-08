#include "src/cyw43_blink_led.h"

#define BLINK_LED_CALLBACK_TIME (3 * 1000)

static bool cyw43_blink_led_process(repeating_timer_t *rt);

static repeating_timer_t timer;

void cyw43_blink_led_init(void) {
    if (!cyw43_is_initialized(&cyw43_state)) {
        if (cyw43_arch_init()) {
            printf("Wi-Fi init failed \n");
            return;
        }
    }
    add_repeating_timer_ms(BLINK_LED_CALLBACK_TIME, cyw43_blink_led_process, NULL, &timer);
}

bool cyw43_blink_led_process(repeating_timer_t *rt) {
    static int blink_led_state = 0;
    if (blink_led_state == 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    } else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    }
    blink_led_state = !blink_led_state;
return true;
}
