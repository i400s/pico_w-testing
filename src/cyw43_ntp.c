#include "pico/cyw43_arch.h"
#include "hardware/rtc.h"
#include "lwip/dns.h"
#include "pico/util/datetime.h"
#include "src/cyw43_ntp.h"

#define NTP_SERVER "pool.ntp.org"
#define NTP_MSG_LEN 48
#define NTP_PORT 123
#define NTP_DELTA 2208988800 // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_TEST_TIME (30 * 1000)
#define NTP_RESEND_TIME (10 * 1000)
#define NTP_CALLBACK_TIME (60 * 60 * 1000)
#define NTP_LOGON_TIMEOUT (30 * 1000)

#define ntp_packet_li(packet)   (uint8_t) ((packet->li_vn_mode & 0xC0) >> 6) // (li   & 11 000 000) >> 6
#define ntp_packet_vn(packet)   (uint8_t) ((packet->li_vn_mode & 0x38) >> 3) // (vn   & 00 111 000) >> 3
#define ntp_packet_mode(packet) (uint8_t) ((packet->li_vn_mode & 0x07) >> 0) // (mode & 00 000 111) >> 0

static repeating_timer_t timer;
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

static void ntp_result(NTP_T* state, int status, time_t *result);
static int64_t ntp_failed_handler(alarm_id_t id, void *user_data);
static void ntp_request(NTP_T *state);
static int64_t ntp_failed_handler(alarm_id_t id, void *user_data);
static void ntp_dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg);
static void ntp_to_timeval(struct ntp_ts_t *ntp, struct timeval *tv);
static void timeval_to_ntp(struct timeval *tv, struct ntp_ts_t *ntp);
static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static NTP_T* cyw43_ntp_get_state(void);
static int32_t cyw43_ntp_initiate_request(void);
static bool cyw43_ntp_process(repeating_timer_t *rt);

static repeating_timer_t timer;

void cyw43_ntp_init() {
    if (!cyw43_is_initialized(&cyw43_state)) {
        if (cyw43_arch_init()) {
            printf("Wi-Fi init failed \n");
            return;
        }
    }
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, NTP_LOGON_TIMEOUT)) {
        printf("Connect to local Wi-Fi failed to initiate \n");
        return;
    }

    cyw43_ntp_initiate_request();

    add_repeating_timer_ms(NTP_CALLBACK_TIME, cyw43_ntp_process, NULL, &timer);
}

bool cyw43_ntp_process(repeating_timer_t *rt) {
        cyw43_ntp_initiate_request();
        return true;
}


// Perform initialisation
NTP_T* cyw43_ntp_get_state(void) {
    static NTP_T *state;
    if (!state) {
        state = (NTP_T*)calloc(1, sizeof(NTP_T));
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
    }
    return state;
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
        printf("rtc(%02d/%02d/%04d %02d:%02d:%02d) \n", t.day, t.month, t.year, t.hour, t.min, t.sec);
        rtc_set_datetime(&t);
    }

    if (state->ntp_resend_alarm > 0) {
        cancel_alarm(state->ntp_resend_alarm);
        state->ntp_resend_alarm = 0;
    }
    state->ntp_test_time = make_timeout_time_ms(NTP_TEST_TIME);
    state->dns_request_sent = false;
}



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
        printf("Addr(%s) ", ipaddr_ntoa(ipaddr));
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
    if (ip_addr_cmp(addr, &state->ntp_server_address) && port == NTP_PORT && p->tot_len == NTP_MSG_LEN &&
        mode == 0x4 && stratum != 0) {
        uint8_t seconds_buf[4] = {0};
        pbuf_copy_partial(p, seconds_buf, sizeof(seconds_buf), 40);
        uint32_t seconds_since_1900 = seconds_buf[0] << 24 | seconds_buf[1] << 16 | seconds_buf[2] << 8 | seconds_buf[3];
        uint32_t seconds_since_1970 = seconds_since_1900 - NTP_DELTA;
        printf("SecsSince1970(%0lu) ", seconds_since_1970);
        time_t epoch = seconds_since_1970;

        uint8_t fract_buf[4] = {0};
        pbuf_copy_partial(p, fract_buf, sizeof(fract_buf), 44);
        uint32_t fractional_seconds = fract_buf[0] << 24 | fract_buf[1] << 16 | fract_buf[2] << 8 | fract_buf[3];
        printf("FracSecs(%0lu) ", fractional_seconds);
        ntp_result(state, 0, &epoch);
    } else {
        printf("invalid ntp response \n");
        ntp_result(state, -1, NULL);
    }
    pbuf_free(p);
}

// Periodically send an ntp request which will be serviced via callbacks.
int32_t cyw43_ntp_initiate_request() {
    NTP_T *state = cyw43_ntp_get_state();
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
