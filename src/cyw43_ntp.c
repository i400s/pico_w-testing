#include "src/cyw43_ntp.h"
#include "lwip/dns.h"


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
NTP_T* ntp_init(void) {
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
int32_t ntp_initiate_request(NTP_T *state) {
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
