#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/structs/timer.h"
#include "hardware/rtc.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/i2c.h"
#include "pico/util/datetime.h"

#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define BACKLIGHT_LED 7
#define TOUCHSCREEN_IRQ 3
#define BACKLIGHT_MAX 0xFF
#define BACKLIGHT_STEP 35

#define I2C0_SCL_PIN 17
#define I2C0_SDA_PIN 16
#define MCP9808_IRQ 4

#define NTP_SERVER "pool.ntp.org"
#define NTP_MSG_LEN 48
#define NTP_PORT 123
#define NTP_DELTA 2208988800 // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_TEST_TIME (30 * 1000)
#define NTP_RESEND_TIME (10 * 1000)

#define ntp_packet_li(packet)   (uint8_t) ((packet->li_vn_mode & 0xC0) >> 6) // (li   & 11 000 000) >> 6
#define ntp_packet_vn(packet)   (uint8_t) ((packet->li_vn_mode & 0x38) >> 3) // (vn   & 00 111 000) >> 3
#define ntp_packet_mode(packet) (uint8_t) ((packet->li_vn_mode & 0x07) >> 0) // (mode & 00 000 111) >> 0

//The bus address is determined by the state of pins A0, A1 and A2 on the MCP9808 board
const uint8_t MCP9808_ADDRESS[4] = {0x18, 0x19, 0x1A, 0x1B};
//hardware registers
const uint8_t REG_POINTER = 0x00;
const uint8_t REG_CONFIG = 0x01;
const uint8_t REG_TEMP_UPPER = 0x02;
const uint8_t REG_TEMP_LOWER = 0x03;
const uint8_t REG_TEMP_CRIT = 0x04;
const uint8_t REG_TEMP_AMB = 0x05;
const uint8_t REG_RESOLUTION = 0x08;

static volatile int backlight_brightness = BACKLIGHT_MAX;
static volatile alarm_id_t alarm_id = 0;

typedef struct NTP_T_ {
    ip_addr_t ntp_server_address;
    bool dns_request_sent;
    struct udp_pcb *ntp_pcb;
    absolute_time_t ntp_test_time;
    alarm_id_t ntp_resend_alarm;
} NTP_T;

// ntp time stamp structure
struct ntp_ts_t {
    uint32_t seconds;
    uint32_t fraction;
};

// ntp udp data record
struct ntp_packet_t {
    u8_t li_vn_mode;
    u8_t stratum;
    u8_t poll;
    u8_t precession;
    u32_t root_delay;
    u32_t root_dispersion;
    u32_t ref_id;
    struct ntp_ts_t ref_timestamp;
    struct ntp_ts_t origin_timestamp;
    struct ntp_ts_t receive_timestamp;
    struct ntp_ts_t transmit_timestamp;
} __attribute__((packed, aligned(1)));

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
void mcp9808_reset_irq(uint8_t i);

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

void mcp9808_set_limits(uint8_t i);

static void mcp9808_init(void) {

    float temp = 0.0;
    int16_t whole = 0;
    int16_t decimal = 0;
    uint16_t result = 0;

    temp = 19.00; // LT
    whole = (int16_t)temp << 4;
    decimal = (int16_t)((temp - (int16_t)temp) / .25) << 2;
    result = whole | decimal;
    printf("LT-%2.2f whole (%d) (%x) ", temp, whole, whole);
    printf("decimal (%d) (%x) ", decimal, decimal);
    printf("result (%d) (%x) ", result, result);
    result = (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
    printf("full (%2.2f) (%d) (%x) \n", temp, result, result);

    temp = 21.75; // LT+hysteresis(1.5)+1.25
    whole = (int16_t)temp << 4;
    decimal = (int16_t)((temp - (int16_t)temp) / .25) << 2;
    result = whole | decimal;
    printf("UT-%2.2f whole (%d) (%x) ", temp, whole, whole);
    printf("decimal (%d) (%x) ", decimal, decimal);
    printf("result (%d) (%x) ", result, result);
    result = (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
    printf("full (%2.2f) (%d) (%x) \n", temp, result, result);

    temp = 24.00; //UT+hysteresis(1.5)+.75
    whole = (int16_t)temp << 4;
    decimal = (int16_t)((temp - (int16_t)temp) / .25) << 2;
    result = whole | decimal;
    printf("CT-%2.2f whole (%d) (%x) ", temp, whole, whole);
    printf("decimal (%d) (%x) ", decimal, decimal);
    printf("result (%d) (%x) ", result, result);
    result = (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
    printf("full (%2.2f) (%d) (%x) \n", temp, result, result);

    gpio_set_irq_enabled(MCP9808_IRQ, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(&gpio_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_pull_up(MCP9808_IRQ);

    //
    for (uint8_t i = 0; i < 4; i++) {
        mcp9808_set_limits(i);
    }
}

void mcp9808_set_limits(uint8_t i) {

    //Set a lower limit of 19.00°C for the temperature
    uint8_t lower_temp_msb = 0x01;
    uint8_t lower_temp_lsb = 0x30;

    //Set an upper limit of 21.75°C for the temperature
    uint8_t upper_temp_msb = 0x01;
    uint8_t upper_temp_lsb = 0x5C;

    //Set a critical limit of 24.00°C for the temperature
    uint8_t crit_temp_msb = 0x01;
    uint8_t crit_temp_lsb = 0x80;

    uint8_t buf[3];

    printf("Init (%d", MCP9808_ADDRESS[i]);

    buf[0] = REG_TEMP_LOWER;
    buf[1] = lower_temp_msb;
    buf[2] = lower_temp_lsb;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":LT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_TEMP_UPPER;
    buf[1] = upper_temp_msb;
    buf[2] = upper_temp_lsb;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":UT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_TEMP_CRIT;
    buf[1] = crit_temp_msb;
    buf[2] = crit_temp_lsb;;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":CT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_RESOLUTION;
    buf[1] = 0x01; // .25°C resolution
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 2, false) <0) {
        printf("*WE*");
    } else {
        printf(":RS-%02x", buf[1]);
    }

    buf[0] = REG_CONFIG;
    buf[1] = 0x02; // 21:Hysteresis 1.5°C
    buf[2] = 0x39; // 5:Interrupt clear 3:Alert 0:interrupt mode
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) < 0) {
        printf("*WE*");
    } else {
        printf(":CN-%02x%02x", buf[1], buf[2]);
    }

    buf[1] = 0;
    buf[2] = 0;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_CONFIG, 1, true) < 0) {
        printf("*WE*");
    } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], &buf[1], 2, false) < 0) {
        printf("*RE*");
    } else {
        printf(":%02x%02x*OK*) ", buf[1], buf[2]);
    }

    printf("\n");

}

void mcp9808_reset_irq(uint8_t i) {
    uint8_t buf[3];
    printf("(%d ", MCP9808_ADDRESS[i]);
    buf[0] = REG_CONFIG;
    buf[1] = 0;
    buf[2] = 0;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_CONFIG, 1, true) < 0) {
        printf("*WE*");
    } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], &buf[1], 2, false) < 0) {
        printf("*RE*");
    } else {
        printf(":%02x%02x*OK*", buf[1], buf[2]);
        if (buf[2] & (1 << 4)) { // 4:Interrupt this device
            buf[2] |= (1 << 5); // 5:Interrupt clear
            printf("\033[0;32m:%02x%02x", buf[1], buf[2]);
            if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) < 0) {
                printf("*WE*");
            } else if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_CONFIG, 1, true) < 0) {
                printf("*WE*");
            } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], &buf[1], 2, false) < 0) {
                printf("*RE*");
            } else {
                printf(":%02x%02x*OK*", buf[1], buf[2]);
            }
            printf("\033[0m");
        }
        printf(") ");
    }

}

void mcp9808_check_limits(uint8_t upper_byte) {

    // Check flags and raise alerts accordingly
    if ((upper_byte & 0x40) == 0x40) { //TA > T-UPPER
        printf("\033[0;32m*UL*\033[0m");
    }
    if ((upper_byte & 0x20) == 0x20) { //TA < T-LOWER
        printf("\033[0;32m*LL*\033[0m");
    }
    if ((upper_byte & 0x80) == 0x80) { //TA >= T-CRIT
        printf("\033[0;32m*CT*\033[0m");
    }
}

float mcp9808_convert_temp(uint8_t upper_byte, uint8_t lower_byte) {

    float temperature;


    //Check if TA <= 0°C and convert to denary accordingly
    if ((upper_byte & 0x10) == 0x10) {
        upper_byte = upper_byte & 0x0F;
        temperature = 256 - (((float) upper_byte * 16) + ((float) lower_byte / 16));
    } else {
        temperature = (((float) upper_byte * 16) + ((float) lower_byte / 16));

    }
    return temperature;
}

void mcp9808_process(void) {

    uint8_t buf[2];
    uint16_t upper_byte;
    uint16_t lower_byte;

    float temperature;

    printf("Temp: ");

    for (uint8_t i = 0; i < 4; i++) {
        // Start reading ambient temperature register for 2 bytes
        if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_TEMP_AMB, 1, true) < 0) {
            printf("*WE*");
            continue;
        } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], buf, 2, false) < 0) {
            printf("*RE*");
            continue;
        }

        upper_byte = buf[0];
        lower_byte = buf[1];

        // printf("UB:%x ", upper_byte);

        //clears flag bits in upper byte
        temperature = mcp9808_convert_temp(upper_byte & 0x1F, lower_byte);
        printf("(%d: %.2f°C", MCP9808_ADDRESS[i], temperature);

        //isolates limit flags in upper byte
        mcp9808_check_limits(upper_byte & 0xE0);

        printf(") ");
    }
    printf("\n");
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

// Called with results of operation
static void ntp_result(NTP_T* state, int status, time_t *result) {
    if (status == 0 && result) {
        // Start on Friday 5th of June 2020 15:45:00
        struct tm *utc = gmtime(result);
        datetime_t t = {
            .year  = utc->tm_year + 1900,
            .month = utc->tm_mon + 1,
            .day   = utc->tm_mday,
            .dotw  = utc->tm_wday, // 0 is Sunday, so 5 is Friday
            .hour  = utc->tm_hour,
            .min   = utc->tm_min,
            .sec   = utc->tm_sec
        };
        printf("got ntp response: %02d/%02d/%04d %02d:%02d:%02d\n", t.day, t.month, t.year,
               t.hour, t.min, t.sec);
        printf("Setting rtc\n");
        rtc_set_datetime(&t);
    }

    if (state->ntp_resend_alarm > 0) {
        cancel_alarm(state->ntp_resend_alarm);
        state->ntp_resend_alarm = 0;
    }
    state->ntp_test_time = make_timeout_time_ms(NTP_TEST_TIME);
    state->dns_request_sent = false;
}

static int64_t ntp_failed_handler(alarm_id_t id, void *user_data);

// Make an NTP request
static void ntp_request(NTP_T *state) {
    // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
    // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
    // these calls are a no-op and can be omitted, but it is a good practice to use them in
    // case you switch the cyw43_arch type later.
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
    uint8_t *req = (uint8_t *) p->payload;
    memset(req, 0, NTP_MSG_LEN);
    req[0] = 0x1b;
    udp_sendto(state->ntp_pcb, p, &state->ntp_server_address, NTP_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
}

static int64_t ntp_failed_handler(alarm_id_t id, void *user_data)
{
    NTP_T* state = (NTP_T*)user_data;
    printf("ntp request failed\n");
    ntp_result(state, -1, NULL);
    return 0;
}

// Call back with a DNS result
static void ntp_dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    NTP_T *state = (NTP_T*)arg;
    if (ipaddr) {
        state->ntp_server_address = *ipaddr;
        printf("ntp address %s\n", ipaddr_ntoa(ipaddr));
        ntp_request(state);
    } else {
        printf("ntp dns request failed\n");
        ntp_result(state, -1, NULL);
    }
}

// 1900/01/01 to 1970/01/01 is NTP_DELTA seconds. Remove those extra seconds to get unix time.
void ntp_to_timeval(struct ntp_ts_t *ntp, struct timeval *tv) {
    tv->tv_sec = ntp->seconds - NTP_DELTA;
    tv->tv_usec = (uint32_t)((double)ntp->fraction * 1.0e6 / (double)(1LL << 32));
}

// 1900/01/01 to 1970/01/01 is NTP_DELTA seconds. Add those extra seconds to get ntp time.
void timeval_to_ntp(struct timeval *tv, struct ntp_ts_t *ntp) {
    ntp->seconds = tv->tv_sec + NTP_DELTA;
    ntp->fraction = (uint32_t)((double)(tv->tv_usec + 1) * (double)(1LL << 32) * 1.0e-6);
}

// NTP data received
static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    NTP_T *state = (NTP_T*)arg;
    uint8_t mode = pbuf_get_at(p, 0) & 0x7;
    uint8_t stratum = pbuf_get_at(p, 1);
    struct ntp_packet_t *ntp_packet;
    ntp_packet = (struct ntp_packet_t *)calloc(1, sizeof(struct ntp_packet_t));
    if (!ntp_packet) {
        printf("failed to allocate ntp_data\n");
        ntp_result(state, -1, NULL);
    } else if (ip_addr_cmp(addr, &state->ntp_server_address) && port == NTP_PORT && p->tot_len == NTP_MSG_LEN &&
        mode == 0x4 && stratum != 0) {
        uint8_t seconds_buf[4] = {0};
        pbuf_copy_partial(p, seconds_buf, sizeof(seconds_buf), 40);
        uint32_t seconds_since_1900 = seconds_buf[0] << 24 | seconds_buf[1] << 16 | seconds_buf[2] << 8 | seconds_buf[3];
        uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
        time_t epoch = seconds_since_1970;
        printf("Seconds since 1970 (%0lu)\n", seconds_since_1970);
        ntp_result(state, 0, &epoch);

        uint8_t fract_buf[4] = {0};
        pbuf_copy_partial(p, fract_buf, sizeof(fract_buf), 44);
        uint32_t fractional_seconds = fract_buf[0] << 24 | fract_buf[1] << 16 | fract_buf[2] << 8 | fract_buf[3];
        printf("Fractional seconds (%0lu)\n", fractional_seconds);

        printf("sizeof(ntp_packet) %0u\n", sizeof(ntp_packet));
        pbuf_copy_partial(p, ntp_packet, sizeof(*ntp_packet), 0);
        printf("mode %0u\n", mode);
        printf("ntp_packet_mode: %0u\n", ntp_packet_mode(ntp_packet));
        // These two fields contain the time-stamp seconds as the packet left the NTP server.
        // The number of seconds correspond to the seconds passed since 1900.
        // ntohl() converts the bit/byte order from the network's to host's "endianness".
        ntp_packet->transmit_timestamp.seconds = ntohl(ntp_packet->transmit_timestamp.seconds); // Time-stamp seconds.
        ntp_packet->transmit_timestamp.fraction = ntohl(ntp_packet->transmit_timestamp.fraction); // Time-stamp fraction of a second.
        printf("seconds since (%0u : %0u)\n", seconds_since_1900, ntp_packet->transmit_timestamp.seconds);
        printf("%0u %0u %0u\n", ntp_packet_li(ntp_packet), ntp_packet_mode(ntp_packet), ntp_packet_vn(ntp_packet));
    } else {
        printf("invalid ntp response\n");
        ntp_result(state, -1, NULL);
    }
    free(ntp_packet);
    pbuf_free(p);
}

// Perform initialisation
static NTP_T* ntp_init(void) {
    NTP_T *state = (NTP_T*)calloc(1, sizeof(NTP_T));
    if (!state) {
        printf("failed to allocate state\n");
        return NULL;
    }
    state->ntp_pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (!state->ntp_pcb) {
        printf("failed to create pcb\n");
        free(state);
        return NULL;
    }
    udp_recv(state->ntp_pcb, ntp_recv, state);
    return state;
}

// Periodically send an ntp request which will be serviced via callbacks.
static int32_t ntp_initiate_request(NTP_T *state) {
    if (absolute_time_diff_us(get_absolute_time(), state->ntp_test_time) < 0 && !state->dns_request_sent) {
        // Set alarm in case udp requests are lost
        state->ntp_resend_alarm = add_alarm_in_ms(NTP_RESEND_TIME, ntp_failed_handler, state, true);

        // cyw43_arch_lwip_begin/end should be used around calls into lwIP to ensure correct locking.
        // You can omit them if you are in a callback from lwIP. Note that when using pico_cyw_arch_poll
        // these calls are a no-op and can be omitted, but it is a good practice to use them in
        // case you switch the cyw43_arch type later.
        cyw43_arch_lwip_begin();
        int err = dns_gethostbyname(NTP_SERVER, &state->ntp_server_address, ntp_dns_found, state);
        cyw43_arch_lwip_end();

        state->dns_request_sent = true;
        if (err == ERR_OK) {
            ntp_request(state); // Cached result
        } else if (err != ERR_INPROGRESS) { // ERR_INPROGRESS means expect a callback
            printf("dns request failed\n");
            ntp_result(state, -1, NULL);
        }
    }
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
    mcp9808_init();

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
