#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "hardware/rtc.h"
#include "src/mcp9808.h"
#include "src/cyw43_ntp.h"
#include "src/msp2807.h"
#include "src/cyw43_blink_led.h"

#define I2C0_SCL_PIN 17
#define I2C0_SDA_PIN 16

void gpio_event_string(char *buf, uint32_t events) {
    static const char *gpio_irq_str[] = {
        "LEVEL_LOW",  // 0x1
        "LEVEL_HIGH", // 0x2
        "EDGE_FALL",  // 0x4
        "EDGE_RISE"   // 0x8
    };

    for (uint i = 0; i < 4; i++) {
        uint mask = (1 << i);
        if (events & mask) {
            // Copy this event string into the user string
            const char *event_str = gpio_irq_str[i];
            while (*event_str != '\0') {
                *buf++ = *event_str++;
            }
            events &= ~mask;

            // If more events add ", "
            if (events) {
                *buf++ = ',';
                *buf++ = ' ';
            }
        }
    }
    *buf++ = '\0';
}

void gpio_callback(uint gpio, uint32_t events) {
    static char event_str[128];
    // Put the GPIO event(s) that just happened into event_str
    // so we can print it
    gpio_event_string(event_str, events);
    printf("GPIO %d %s ", gpio, event_str);

    switch(gpio)
    {
        case TOUCHSCREEN_IRQ:
            msp2807_reset_irq();
            break;
        case MCP9808_IRQ:
            mcp9808_reset_irq();
            break;
    }
    printf("\n");
}

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    printf("Timer %d fired!\n", (int) id);

    backlight_check_timer(id);

    // Can return a value here in us to fire in the future
    return 0;
}

static void i2c0_init(void) {
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SCL_PIN);
    gpio_pull_up(I2C0_SDA_PIN);
}



int main()
{
    // Must be set to zero when debugging else tick tests cause infinite loops.
    // Additional information suggests that the issue is caused by debugging both
    // cores (latest openocd default) when in reality only one is being worked on.
    // As there is only one timer, when either core is halted the timer stops.
#ifdef DBGPAUSE
    timer_hw->dbgpause = 0;
#endif

    stdio_init_all();

    printf("\n\nPico is alive. \n");

    printf("Initialising rtc. \n");
    rtc_init();

    printf("Initialising i2c0. \n");
    i2c0_init();

    printf("Initialising backlight. \n");
    backlight_init(alarm_callback);

    printf("Initialising touch screen. \n");
    touchscreen_init(gpio_callback);

    printf("Initialising mcp9808 devices. \n");
    mcp9808_init(gpio_callback);

    printf("Initialising cyw43 for blink led \n");
    cyw43_blink_led_init();

    printf("Initialising cyw43 for ntp \n");
    cyw43_ntp_init();

    while (true) {
        tight_loop_contents;
    }
}
