/*
 * dns_fwd.c — DNS forwarder with cache and captive-portal fallback.
 * Lifted nearly verbatim from satura-bridge (pan_wifi_bridge.c v0.0.9).
 *
 * The phone gets 192.168.7.1 as its DNS server via IPCP. While WiFi is up,
 * queries are forwarded upstream (with a small cache); while it is down,
 * A queries get answered with 192.168.7.1 so any http:// page the phone
 * opens lands on the setup UI.
 */

#include <string.h>
#include <errno.h>

#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "gateway.h"

static const char *TAG = "dns_fwd";

#define DNS_PORT            53
#define DNS_TIMEOUT_MS      500
#define DNS_CACHE_SIZE      16
#define DNS_MAX_PACKET      512
#define FALLBACK_DNS        "8.8.8.8"

/* Watchdog: DNS is considered hung after this many ms without a loop pass,
 * confirmed over DNS_WATCHDOG_TICKS heartbeats. */
#define DNS_WATCHDOG_MS     15000
#define DNS_WATCHDOG_TICKS  2

typedef struct {
    bool     valid;
    uint16_t hash;
    uint16_t qlen;
    uint16_t rlen;
    uint32_t saved_ms;
    uint8_t  query[DNS_MAX_PACKET];
    uint8_t  reply[DNS_MAX_PACKET];
} dns_cache_entry_t;

/* dns_srv_sock / dns_ext_sock: written only by dns_server_task itself.
 * dns_task_handle: written by watchdog (under state_mux) and by dns task on exit.
 * dns_restart_flag: set by watchdog, cleared by dns task — volatile is enough. */
static int dns_srv_sock = -1;
static int dns_ext_sock = -1;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];
static uint8_t dns_cache_next = 0;
static TaskHandle_t dns_task_handle = NULL;
static volatile uint32_t dns_last_alive_ms = 0;
static volatile bool     dns_restart_flag  = false;

static void dns_server_task(void *arg);

// ============================================================
// Cache helpers
// ============================================================

static uint16_t dns_query_hash(const uint8_t *q, int qlen) {
    uint32_t h = 0;
    for (int i = 12; i < qlen && i < 40; i++) h = h * 31 + q[i];
    return (uint16_t)(h ^ (h >> 16));
}

static bool dns_cache_lookup(const uint8_t *query, int qlen,
                              uint8_t *reply, int *rlen) {
    if (qlen < 12) return false;
    uint16_t qhash = dns_query_hash(query, qlen);
    uint32_t now   = (uint32_t)(esp_timer_get_time() / 1000ULL);
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_cache_entry_t *e = &dns_cache[i];
        if (!e->valid || e->hash != qhash || e->qlen != (uint16_t)qlen) continue;
        if ((uint32_t)(now - e->saved_ms) > 60000) { e->valid = false; continue; }
        if (qlen > 12 && memcmp(e->query + 12, query + 12, qlen - 12) != 0) continue;
        memcpy(reply, e->reply, e->rlen);
        reply[0] = query[0];
        reply[1] = query[1];
        *rlen = e->rlen;
        return true;
    }
    return false;
}

static void dns_cache_store(const uint8_t *query, int qlen,
                             const uint8_t *reply, int rlen) {
    if (qlen < 12 || qlen > DNS_MAX_PACKET ||
        rlen < 12 || rlen > DNS_MAX_PACKET) return;
    dns_cache_entry_t *e = &dns_cache[dns_cache_next];
    dns_cache_next = (dns_cache_next + 1) % DNS_CACHE_SIZE;
    e->valid    = true;
    e->hash     = dns_query_hash(query, qlen);
    e->qlen     = (uint16_t)qlen;
    e->rlen     = (uint16_t)rlen;
    e->saved_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    memcpy(e->query, query, qlen);
    memcpy(e->reply, reply, rlen);
    /* Zero out txid so hash/comparison ignores it */
    e->query[0] = e->query[1] = e->reply[0] = e->reply[1] = 0;
}

/* Only generate a captive reply for A-type queries.
 * AAAA / PTR / MX / SRV etc. get NXDOMAIN so the client
 * doesn't loop waiting for a non-A answer and retries A. */
static int dns_make_captive_reply(const uint8_t *query, int qlen,
                                   uint8_t *reply, int rmax) {
    if (qlen < 12 || rmax < 28) return 0;

    /* Locate qtype — walk the QNAME labels first. */
    int off = 12;
    while (off < qlen - 4) {
        uint8_t len = query[off];
        if (len == 0) { off++; break; }                /* root label */
        if ((len & 0xC0) == 0xC0) { off += 2; break; } /* pointer (unlikely) */
        off += 1 + len;
    }
    if (off + 4 > qlen) return 0;
    uint16_t qtype = (query[off] << 8) | query[off + 1];

    if (qtype != 0x0001) {
        /* NXDOMAIN (RCODE=3) so the client moves on quickly */
        if (rmax < 12) return 0;
        if (qlen > 200) qlen = 200;
        memcpy(reply, query, qlen > 12 ? 12 : qlen);
        reply[0] = query[0]; reply[1] = query[1];
        reply[2] = 0x81; reply[3] = 0x83; /* QR=1 AA=0 RCODE=3 */
        reply[4] = 0x00; reply[5] = 0x01; /* QDCOUNT=1 */
        reply[6] = 0x00; reply[7] = 0x00; /* ANCOUNT=0 */
        reply[8] = 0x00; reply[9] = 0x00;
        reply[10]= 0x00; reply[11]= 0x00;
        int rlen = 12;
        int qsz  = qlen - 12;
        if (qsz > 0 && rlen + qsz <= rmax) {
            memcpy(reply + rlen, query + 12, qsz);
            rlen += qsz;
        }
        return rlen;
    }

    /* Build an A reply pointing at the gateway */
    if (qlen > 200) qlen = 200;
    reply[0] = query[0]; reply[1] = query[1];
    reply[2] = 0x81; reply[3] = 0x80;
    reply[4] = 0x00; reply[5] = 0x01;
    reply[6] = 0x00; reply[7] = 0x01;
    reply[8] = 0x00; reply[9] = 0x00;
    reply[10]= 0x00; reply[11]= 0x00;
    int rlen   = 12;
    int qsection = qlen - 12;
    if (qsection <= 0 || rlen + qsection + 16 > rmax) return 0;
    memcpy(reply + rlen, query + 12, qsection);
    rlen += qsection;
    reply[rlen++] = 0xC0; reply[rlen++] = 0x0C;
    reply[rlen++] = 0x00; reply[rlen++] = 0x01;
    reply[rlen++] = 0x00; reply[rlen++] = 0x01;
    reply[rlen++] = 0x00; reply[rlen++] = 0x00;
    reply[rlen++] = 0x00; reply[rlen++] = 0x3C;
    reply[rlen++] = 0x00; reply[rlen++] = 0x04;
    reply[rlen++] = GW_IP0; reply[rlen++] = GW_IP1;
    reply[rlen++] = GW_IP2; reply[rlen++] = GW_IP3;
    return rlen;
}

/* Only generate a SERVFAIL for queries that fail to resolve while WiFi is
 * up (e.g. a transient upstream timeout). Handing back the gateway's own
 * IP in that case — like the captive fallback does — would poison the
 * phone's DNS cache with a bogus mapping for a real domain, which then
 * keeps landing on the local web UI even once the upstream is reachable
 * again. */
static int dns_make_servfail_reply(const uint8_t *query, int qlen,
                                    uint8_t *reply, int rmax) {
    if (qlen < 12 || rmax < 12) return 0;
    if (qlen > 200) qlen = 200;
    memcpy(reply, query, qlen > 12 ? 12 : qlen);
    reply[0] = query[0]; reply[1] = query[1];
    reply[2] = 0x81; reply[3] = 0x82; /* QR=1 AA=0 RCODE=2 (SERVFAIL) */
    reply[4] = 0x00; reply[5] = 0x01; /* QDCOUNT=1 */
    reply[6] = 0x00; reply[7] = 0x00; /* ANCOUNT=0 */
    reply[8] = 0x00; reply[9] = 0x00;
    reply[10]= 0x00; reply[11]= 0x00;
    int rlen = 12;
    int qsz  = qlen - 12;
    if (qsz > 0 && rlen + qsz <= rmax) {
        memcpy(reply + rlen, query + 12, qsz);
        rlen += qsz;
    }
    return rlen;
}

static bool dns_forward(int ext_sock, struct sockaddr_in *ext_dns,
                         const uint8_t *query, int qlen,
                         uint8_t *reply, int *rlen) {
    if (sendto(ext_sock, query, qlen, 0,
               (struct sockaddr *)ext_dns, sizeof(*ext_dns)) < 0) return false;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(ext_sock, reply, DNS_MAX_PACKET, 0,
                     (struct sockaddr *)&from, &fl);
    if (n >= 12 && reply[0] == query[0] && reply[1] == query[1]) {
        *rlen = n; return true;
    }
    return false;
}

static void dns_get_upstream(struct sockaddr_in *out) {
    out->sin_family = AF_INET;
    out->sin_port   = htons(53);
    esp_netif_dns_info_t dns_info = {0};
    esp_netif_t *sta = (esp_netif_t *)wifi_get_sta_netif();
    if (sta &&
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK
        && dns_info.ip.u_addr.ip4.addr != 0) {
        out->sin_addr.s_addr = dns_info.ip.u_addr.ip4.addr;
    } else {
        inet_pton(AF_INET, FALLBACK_DNS, &out->sin_addr);
    }
}

// ============================================================
// Server task
// ============================================================

/* The task owns its sockets for its full lifetime. Restart is signalled via
 * dns_restart_flag; the task closes its own sockets and deletes itself. */
static void dns_server_task(void *arg) {
    (void)arg;

    dns_restart_flag = false;

    dns_srv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (dns_srv_sock < 0) {
        ESP_LOGE(TAG, "failed to open srv sock: %d", errno);
        goto out_no_socks;
    }
    dns_ext_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (dns_ext_sock < 0) {
        ESP_LOGE(TAG, "failed to open ext sock: %d", errno);
        close(dns_srv_sock);
        dns_srv_sock = -1;
        goto out_no_socks;
    }

    struct timeval tv  = {1, 0};
    struct timeval tv2 = {0, DNS_TIMEOUT_MS * 1000};
    setsockopt(dns_srv_sock, SOL_SOCKET, SO_RCVTIMEO, &tv,  sizeof(tv));
    setsockopt(dns_ext_sock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(dns_srv_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(dns_srv_sock);
        close(dns_ext_sock);
        dns_srv_sock = dns_ext_sock = -1;
        goto out_no_socks;
    }

    ESP_LOGI(TAG, "task started");

    while (1) {
        if (dns_restart_flag) {
            ESP_LOGW(TAG, "restart flag — shutting down task");
            break;
        }

        dns_last_alive_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        uint8_t query_buf[DNS_MAX_PACKET], reply_buf[DNS_MAX_PACKET];
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int qlen = recvfrom(dns_srv_sock, query_buf, sizeof(query_buf),
                            0, (struct sockaddr *)&client, &clen);
        if (qlen < 12) continue;

        struct sockaddr_in ext_dns;
        dns_get_upstream(&ext_dns);

        bool wifi_up = get_wifi_connected();
        int rlen = 0;
        bool have_reply = dns_cache_lookup(query_buf, qlen, reply_buf, &rlen);
        if (!have_reply) {
            /* Only forward if WiFi is up, otherwise fall through to captive */
            if (wifi_up &&
                dns_forward(dns_ext_sock, &ext_dns,
                            query_buf, qlen, reply_buf, &rlen)) {
                dns_cache_store(query_buf, qlen, reply_buf, rlen);
                have_reply = true;
            }
        }
        if (!have_reply) {
            /* WiFi up but the upstream query failed/timed out: SERVFAIL,
             * not the gateway's own IP — see dns_make_servfail_reply(). */
            rlen = wifi_up
                 ? dns_make_servfail_reply(query_buf, qlen,
                                           reply_buf, sizeof(reply_buf))
                 : dns_make_captive_reply(query_buf, qlen,
                                          reply_buf, sizeof(reply_buf));
        }
        if (rlen > 0) {
            sendto(dns_srv_sock, reply_buf, rlen, 0,
                   (struct sockaddr *)&client, clen);
        }
    }

    close(dns_srv_sock);
    close(dns_ext_sock);
    dns_srv_sock = dns_ext_sock = -1;
out_no_socks:
    taskENTER_CRITICAL(&state_mux);
    dns_task_handle = NULL;
    taskEXIT_CRITICAL(&state_mux);
    vTaskDelete(NULL);
}

// ============================================================
// Public API
// ============================================================

void dns_fwd_start(void) {
    dns_last_alive_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    TaskHandle_t h = NULL;
    gw_safe_task_create(dns_server_task, "dns", 4096, NULL, 6, &h);
    taskENTER_CRITICAL(&state_mux);
    dns_task_handle = h;
    taskEXIT_CRITICAL(&state_mux);
}

/* Called from the watchdog heartbeat: restart the DNS task if it hung. */
void dns_fwd_watchdog_check(void) {
    static uint32_t dns_stuck_count = 0;

    TaskHandle_t dns_h;
    taskENTER_CRITICAL(&state_mux);
    dns_h = dns_task_handle;
    taskEXIT_CRITICAL(&state_mux);

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (dns_h != NULL && (now - dns_last_alive_ms) > DNS_WATCHDOG_MS) {
        if (++dns_stuck_count >= DNS_WATCHDOG_TICKS) {
            ESP_LOGE(TAG, "[WDT] DNS hang! Signalling restart...");

            dns_restart_flag = true;
            taskENTER_CRITICAL(&state_mux);
            dns_task_handle = NULL;   /* prevent double-signal */
            taskEXIT_CRITICAL(&state_mux);

            /* Give the task one SO_RCVTIMEO (1 s) to notice the flag */
            vTaskDelay(pdMS_TO_TICKS(1500));

            memset(dns_cache, 0, sizeof(dns_cache));
            dns_cache_next = 0;
            dns_last_alive_ms = now;

            dns_fwd_start();
            dns_stuck_count = 0;
        }
    } else {
        dns_stuck_count = 0;
    }
}
