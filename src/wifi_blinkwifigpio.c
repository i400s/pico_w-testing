#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"
#include "pico/util/datetime.h"

#include "src/mcp9808.h"
#include "src/ntp_cyw43.h"

#define BACKLIGHT_LED 7
#define TOUCHSCREEN_IRQ 3
#define BACKLIGHT_MAX 0xFF
#define BACKLIGHT_STEP 35

#define I2C0_SCL_PIN 17
#define I2C0_SDA_PIN 16

static volatile int backlight_brightness = BACKLIGHT_MAX;
static volatile alarm_id_t alarm_id = 0;


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

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    printf("Timer %d fired!\n", (int) id);

    // if this timer firing is the one set by the touchscreen event
    // then the timer is complete so it can be cleared.
    if (id == alarm_id) {
        printf("Alarm complete for id: %d\n", alarm_id);
        alarm_id = 0;
    }
    // Can return a value here in us to fire in the future
    return 0;
}

static void backlight_init(void) {
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

static char event_str[128];

void gpio_event_string(char *buf, uint32_t events);

void gpio_callback(uint gpio, uint32_t events) {

    // Put the GPIO event(s) that just happened into event_str
    // so we can print it
    gpio_event_string(event_str, events);
    printf("GPIO %d %s ", gpio, event_str);

    switch(gpio)
    {
        case TOUCHSCREEN_IRQ:
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
            break;
        case MCP9808_IRQ:
            for (uint8_t i = 0; i < 4; i++) {
                mcp9808_reset_irq(i);
            }
            break;
    }
    printf("\n");
}

static void touchscreen_init(void) {
    gpio_set_irq_enabled(TOUCHSCREEN_IRQ, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(&gpio_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_pull_up(TOUCHSCREEN_IRQ);
}

static void i2c0_init(void) {
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SCL_PIN);
    gpio_pull_up(I2C0_SDA_PIN);
}


static const char *gpio_irq_str[] = {
        "LEVEL_LOW",  // 0x1
        "LEVEL_HIGH", // 0x2
        "EDGE_FALL",  // 0x4
        "EDGE_RISE"   // 0x8
};

void gpio_event_string(char *buf, uint32_t events) {
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

    printf("Pico is alive. \n");

    /*const uint LED_PIN = BACKLIGHT_LED;
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    while (true) {
        gpio_put(LED_PIN, 1);
        sleep_ms(250);
        gpio_put(LED_PIN, 0);
        sleep_ms(250);
    }*/

    printf("Initialising backlight. \n");
    backlight_init();

    printf("Initialising touch screen. \n");
    touchscreen_init();

    printf("Initialising i2c0. \n");
    i2c0_init();

    printf("Initialising mcp9808 devices. \n");
    mcp9808_init(&gpio_callback);

    char datetime_buf[256];
    char *datetime_str = &datetime_buf[0];

    // Start on Friday 5th of June 2020 15:45:00
    datetime_t t = {
            .year  = 2020,
            .month = 06,
            .day   = 05,
            .dotw  = 5, // 0 is Sunday, so 5 is Friday
            .hour  = 15,
            .min   = 45,
            .sec   = 00
    };

    // Start the RTC
    rtc_init();
    rtc_set_datetime(&t);

    // clk_sys is >2000x faster than clk_rtc, so datetime is not updated immediately when rtc_get_datetime() is called.
    // tbe delay is up to 3 RTC clock cycles (which is 64us with the default clock settings)
    sleep_us(64);

    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed");
        return -1;
    }

    cyw43_arch_enable_sta_mode();

    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
        printf("failed to connect\n");
        return -1;
    }

    // Initialise ntp communication chanel
    NTP_T *state = ntp_init();
    if (!state) {
        printf("Failed to initialise ntp\n");
        return -1;
    }

    while (true) {

        ntp_initiate_request(state);

        rtc_get_datetime(&t);
        datetime_to_str(datetime_str, sizeof(datetime_buf), &t);
        printf("%s      \n", datetime_str);

        mcp9808_process();

        // printf("Blink LED ON\n");
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(1250);

        // printf("Blink LED OFF\n");
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(1250);
    }

    // While we will never get here, this is the clean up.
    free(state);
    cyw43_arch_deinit();

}
