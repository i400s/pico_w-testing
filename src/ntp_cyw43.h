#ifndef _NTP_CYW43_H
#define _NTP_CYW43_H

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/rtc.h"

#define NTP_SERVER "pool.ntp.org"
#define NTP_MSG_LEN 48
#define NTP_PORT 123
#define NTP_DELTA 2208988800 // seconds between 1 Jan 1900 and 1 Jan 1970
#define NTP_TEST_TIME (30 * 1000)
#define NTP_RESEND_TIME (10 * 1000)

#define ntp_packet_li(packet)   (uint8_t) ((packet->li_vn_mode & 0xC0) >> 6) // (li   & 11 000 000) >> 6
#define ntp_packet_vn(packet)   (uint8_t) ((packet->li_vn_mode & 0x38) >> 3) // (vn   & 00 111 000) >> 3
#define ntp_packet_mode(packet) (uint8_t) ((packet->li_vn_mode & 0x07) >> 0) // (mode & 00 000 111) >> 0

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
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    uint8_t precession;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    struct ntp_ts_t ref_timestamp;
    struct ntp_ts_t origin_timestamp;
    struct ntp_ts_t receive_timestamp;
    struct ntp_ts_t transmit_timestamp;
} __attribute__((packed, aligned(1)));

static void ntp_result(NTP_T* state, int status, time_t *result);
static int64_t ntp_failed_handler(alarm_id_t id, void *user_data);
static void ntp_request(NTP_T *state);
static int64_t ntp_failed_handler(alarm_id_t id, void *user_data);
static void ntp_dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg);
static void ntp_to_timeval(struct ntp_ts_t *ntp, struct timeval *tv);
static void timeval_to_ntp(struct timeval *tv, struct ntp_ts_t *ntp);
static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
NTP_T* ntp_init(void);
int32_t ntp_initiate_request(NTP_T *state);


#endif