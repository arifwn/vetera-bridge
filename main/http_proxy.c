/*
 * http_proxy.c — send the phone's web traffic through an upstream HTTP proxy.
 *
 * With the proxy on, every TCP connection the phone opens to port 80 or 443
 * of any host but the bridge itself goes to a configured proxy instead, with
 * nothing to set up on the phone. The proxies this is meant for (15pmm01.com
 * and similar retro-phone services) are explicit proxies: they want
 * "GET http://host/path" and "CONNECT host:443", not the origin-form request
 * a browser sends to a server it thinks it is talking to directly. A plain
 * DNAT cannot change that, so the bridge terminates the phone's connection
 * itself and talks proxy protocol upstream:
 *
 *  1. Input hook (tcpip task, proxy_ip4_input_hook): a phone SYN to
 *     x.x.x.x:80 or :443 gets its destination rewritten to the bridge's own
 *     relay port (192.168.7.1:3080 or :3443), and the original destination is
 *     kept in a small flow table keyed by the phone's source port. Later
 *     segments of the same flow get the same rewrite.
 *  2. Output wrapper (tcpip task, proxy_netif_output): replies leaving the
 *     relay ports get the original destination put back as their source, so
 *     the phone sees x.x.x.x:80 answering as if nothing happened.
 *  3. Relay task: accepts on the relay ports, reads the request head (HTTP)
 *     or the ClientHello (HTTPS), connects to the proxy, sends the rewritten
 *     head or a CONNECT, then copies bytes both ways.
 *
 * A preset marked http_only (a proxy that refuses CONNECT) leaves port 443
 * alone: the phone's HTTPS goes direct, as with the proxy off.
 *
 * Traffic to 192.168.7.1 (web UI, DNS) is never touched, and nothing is
 * intercepted while WiFi is down, so the captive fallback to the web UI keeps
 * working.
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/def.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/tcp.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include "gateway.h"
#include "proxy_hook.h"

static const char *TAG = "proxy";

#define RELAY_HTTP_PORT     3080
#define RELAY_TLS_PORT      3443

#define FLOW_MAX            16
#define SESS_MAX            4
#define UP_SZ               2048            /* request head / phone -> proxy */
#define UP_CAP              (UP_SZ + 512)   /* room for the rewritten head   */
#define DOWN_SZ             1536            /* proxy -> phone                */
#define SETUP_MS            10000           /* each setup stage              */
#define IDLE_MS             120000          /* splicing, no bytes either way */
/* Free heap needed to take on another session. With a phone on PPP the
 * bridge idles at ~30 KB free; the heartbeat only warns below 24 KB and
 * reboots below 8 KB. */
#define MIN_FREE_HEAP       20480

#define NVS_NAMESPACE       "gw"
#define NVS_KEY_ON          "px_on"
#define NVS_KEY_HOST        "px_host"
#define NVS_KEY_PORT        "px_port"

#define GW_ADDR_N    PP_HTONL(LWIP_MAKEU32(GW_IP0, GW_IP1, GW_IP2, GW_IP3))
#define PHONE_ADDR_N PP_HTONL(LWIP_MAKEU32(GW_IP0, GW_IP1, GW_IP2, GW_PEER_IP3))

/* From https://15pmm01.com/wap/en/. Only the HTTP proxies: the WAP gateways
 * there (port 9201) speak WSP over UDP, not HTTP. Then RetroWEB, from
 * http://retroweb.bjrno.com/, which picks a page profile from the browser's
 * User-Agent (passed through unchanged by http_rewrite) and answers CONNECT
 * with 400, so it is HTTP only. */
static const proxy_preset_t presets[] = {
    { "HTTP Pass",       "176.103.221.33", 3130, "suggested for most vintage phones", false },
    { "HTTP Lite",       "176.103.221.33", 3129, "low-memory phones, text output", false },
    { "HTTP Extra Lite", "176.103.221.33", 3131, "very memory-limited phones, text output", false },
    { "HTTP WML",        "176.103.221.33", 3132, "phones without HTML support", false },
    { "HTTPS",           "176.103.221.33", 3128, "needs its root CA installed on the phone", false },
    { "HTTPS Legacy",    "176.103.221.33", 3127, "fallback for 3128, its own root CA", false },
    { "RetroWEB",        "retroweb.bjrno.com", 8081,
      "simplified HTML tuned to the phone's browser; HTTP only, HTTPS goes direct", true },
};

/* true if host:port is a preset that can't tunnel HTTPS */
static bool host_http_only(const char *host, uint16_t port) {
    for (int i = 0; i < proxy_preset_count(); i++) {
        if (presets[i].port == port && strcmp(presets[i].host, host) == 0)
            return presets[i].http_only;
    }
    return false;
}

int proxy_preset_count(void) {
    return (int)(sizeof(presets) / sizeof(presets[0]));
}

const proxy_preset_t *proxy_preset_get(int i) {
    return (i >= 0 && i < proxy_preset_count()) ? &presets[i] : NULL;
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
// Settings (under state_mux)
// ============================================================

static bool     px_on = false;
static char     px_host[PROXY_HOST_MAX];
static uint16_t px_port = 0;
static volatile bool px_ready = false;      /* on, with a host and port */
static volatile bool px_tls = false;        /* ready, and port 443 goes too */
static volatile int  active_sessions = 0;

void proxy_get(bool *on, char *host, int host_len, uint16_t *port) {
    taskENTER_CRITICAL(&state_mux);
    *on = px_on;
    strlcpy(host, px_host, host_len);
    *port = px_port;
    taskEXIT_CRITICAL(&state_mux);
}

void proxy_set(bool on, const char *host, uint16_t port) {
    taskENTER_CRITICAL(&state_mux);
    px_on = on;
    strlcpy(px_host, host, sizeof(px_host));
    px_port = port;
    px_ready = px_on && px_host[0] && px_port;
    px_tls = px_ready && !host_http_only(px_host, px_port);
    taskEXIT_CRITICAL(&state_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_ON, on ? 1 : 0);
        nvs_set_str(h, NVS_KEY_HOST, host);
        nvs_set_u16(h, NVS_KEY_PORT, port);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "proxy %s, %s:%u", on ? "on" : "off", host, (unsigned)port);
}

int proxy_active_sessions(void) {
    return active_sessions;
}

static void settings_load(void) {
    nvs_handle_t h;
    uint8_t on = 0;
    uint16_t port = 0;
    char host[PROXY_HOST_MAX] = "";
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(host);
        nvs_get_u8(h, NVS_KEY_ON, &on);
        if (nvs_get_str(h, NVS_KEY_HOST, host, &len) != ESP_OK) host[0] = '\0';
        nvs_get_u16(h, NVS_KEY_PORT, &port);
        nvs_close(h);
    }
    taskENTER_CRITICAL(&state_mux);
    px_on = on != 0;
    strlcpy(px_host, host, sizeof(px_host));
    px_port = port;
    px_ready = px_on && px_host[0] && px_port;
    px_tls = px_ready && !host_http_only(px_host, px_port);
    taskEXIT_CRITICAL(&state_mux);
}

// ============================================================
// Flow table (tcpip task + relay task, under flow_mux)
// ============================================================

typedef enum { FLOW_FREE = 0, FLOW_ACTIVE, FLOW_CLOSED } flow_state_t;

typedef struct {
    flow_state_t state;
    uint16_t phone_port;    /* host order */
    uint16_t relay_port;    /* host order: RELAY_HTTP_PORT or RELAY_TLS_PORT */
    uint16_t orig_port;     /* host order: 80 or 443 */
    uint32_t orig_ip;       /* network order */
    uint32_t stamp_ms;      /* last packet, or when the relay closed it */
} flow_t;

static flow_t flows[FLOW_MAX];
static portMUX_TYPE flow_mux = portMUX_INITIALIZER_UNLOCKED;

static flow_t *flow_find(uint16_t phone_port) {
    for (int i = 0; i < FLOW_MAX; i++) {
        if (flows[i].state != FLOW_FREE && flows[i].phone_port == phone_port)
            return &flows[i];
    }
    return NULL;
}

/* A slot for a new flow: free first, then the oldest closed one (lwIP's
 * TIME_WAIT side still needs it for a while, but less than a new flow
 * does), then an active one that has been idle past IDLE_MS. */
static flow_t *flow_slot(uint32_t now) {
    flow_t *closed = NULL, *idle = NULL;
    for (int i = 0; i < FLOW_MAX; i++) {
        flow_t *f = &flows[i];
        if (f->state == FLOW_FREE) return f;
        uint32_t age = now - f->stamp_ms;
        if (f->state == FLOW_CLOSED) {
            if (!closed || age > now - closed->stamp_ms) closed = f;
        } else if (age > IDLE_MS) {
            if (!idle || age > now - idle->stamp_ms) idle = f;
        }
    }
    return closed ? closed : idle;
}

static bool flow_lookup(uint16_t phone_port, uint16_t relay_port,
                        uint32_t *orig_ip, uint16_t *orig_port) {
    bool ok = false;
    taskENTER_CRITICAL(&flow_mux);
    flow_t *f = flow_find(phone_port);
    if (f && f->state == FLOW_ACTIVE && f->relay_port == relay_port) {
        *orig_ip = f->orig_ip;
        *orig_port = f->orig_port;
        ok = true;
    }
    taskEXIT_CRITICAL(&flow_mux);
    return ok;
}

static void flow_mark_closed(uint16_t phone_port) {
    taskENTER_CRITICAL(&flow_mux);
    flow_t *f = flow_find(phone_port);
    if (f) {
        f->state = FLOW_CLOSED;
        f->stamp_ms = now_ms();
    }
    taskEXIT_CRITICAL(&flow_mux);
}

// ============================================================
// Header rewriting (tcpip task)
// ============================================================

/* RFC 1624 incremental checksum update. All values are raw 16-bit words as
 * they sit in the packet; one's complement sums don't care about byte order
 * as long as every word is read the same way. */
static uint16_t csum_adj(uint16_t sum, uint16_t old, uint16_t new_) {
    uint32_t s = (uint16_t)~sum;
    s += (uint16_t)~old;
    s += new_;
    s = (s & 0xffff) + (s >> 16);
    s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static uint16_t csum_adj32(uint16_t sum, uint32_t old, uint32_t new_) {
    sum = csum_adj(sum, (uint16_t)(old >> 16), (uint16_t)(new_ >> 16));
    return csum_adj(sum, (uint16_t)old, (uint16_t)new_);
}

/* IPv4 TCP packet with both headers in the first pbuf, not a fragment.
 * The input hook runs before lwIP validates anything, so check by hand. */
static bool tcp_headers(struct pbuf *p, struct ip_hdr **ip, struct tcp_hdr **th) {
    if (p->len < IP_HLEN) return false;
    struct ip_hdr *h = (struct ip_hdr *)p->payload;
    if (IPH_V(h) != 4 || IPH_PROTO(h) != IP_PROTO_TCP) return false;
    uint16_t hl = IPH_HL_BYTES(h);
    if (hl < IP_HLEN || p->len < hl + TCP_HLEN) return false;
    if ((IPH_OFFSET(h) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0) return false;
    *ip = h;
    *th = (struct tcp_hdr *)((uint8_t *)h + hl);
    return true;
}

static void rewrite_dst(struct ip_hdr *ip, struct tcp_hdr *th,
                        uint32_t new_ip, uint16_t new_port_n) {
    uint32_t old_ip = ip->dest.addr;
    uint16_t old_port = th->dest;
    ip->dest.addr = new_ip;
    th->dest = new_port_n;
    IPH_CHKSUM_SET(ip, csum_adj32(IPH_CHKSUM(ip), old_ip, new_ip));
    th->chksum = csum_adj(csum_adj32(th->chksum, old_ip, new_ip),
                          old_port, new_port_n);
}

static void rewrite_src(struct ip_hdr *ip, struct tcp_hdr *th,
                        uint32_t new_ip, uint16_t new_port_n) {
    uint32_t old_ip = ip->src.addr;
    uint16_t old_port = th->src;
    ip->src.addr = new_ip;
    th->src = new_port_n;
    IPH_CHKSUM_SET(ip, csum_adj32(IPH_CHKSUM(ip), old_ip, new_ip));
    th->chksum = csum_adj(csum_adj32(th->chksum, old_ip, new_ip),
                          old_port, new_port_n);
}

int proxy_ip4_input_hook(struct pbuf *p, struct netif *inp) {
    if (inp == NULL || inp != ppp_link_netif()) return 0;

    struct ip_hdr *ip;
    struct tcp_hdr *th;
    if (!tcp_headers(p, &ip, &th)) return 0;
    if (ip->src.addr != PHONE_ADDR_N || ip->dest.addr == GW_ADDR_N) return 0;

    uint16_t dport = lwip_ntohs(th->dest);
    if (dport != 80 && dport != 443) return 0;
    uint16_t sport = lwip_ntohs(th->src);
    uint16_t relay = dport == 80 ? RELAY_HTTP_PORT : RELAY_TLS_PORT;
    bool syn = (TCPH_FLAGS(th) & (TCP_SYN | TCP_ACK)) == TCP_SYN;
    /* New flows only while there is somewhere to send them, and port 443
     * only if the proxy can tunnel it. Read before taking flow_mux:
     * get_wifi_connected takes state_mux. */
    bool intercept = syn && px_ready && (dport == 80 || px_tls) &&
                     get_wifi_connected();
    uint32_t dst = ip->dest.addr;
    uint32_t now = now_ms();
    bool match = false;

    taskENTER_CRITICAL(&flow_mux);
    flow_t *f = flow_find(sport);
    bool same = f && f->orig_ip == dst && f->orig_port == dport;
    if (syn && f && (!same || f->state == FLOW_CLOSED)) {
        /* The phone reused the port: a new connection, the old flow is gone */
        f->state = FLOW_FREE;
        f = NULL;
        same = false;
    }
    if (same) {
        match = true;
    } else if (intercept) {
        f = flow_slot(now);
        if (f) {
            f->state = FLOW_ACTIVE;
            f->phone_port = sport;
            f->relay_port = relay;
            f->orig_port = dport;
            f->orig_ip = dst;
            match = true;
        }
    }
    if (match) f->stamp_ms = now;
    taskEXIT_CRITICAL(&flow_mux);

    if (match) rewrite_dst(ip, th, GW_ADDR_N, lwip_htons(relay));
    return 0;
}

static netif_output_fn orig_output = NULL;

static err_t proxy_netif_output(struct netif *n, struct pbuf *p,
                                const ip4_addr_t *dst) {
    struct ip_hdr *ip;
    struct tcp_hdr *th;
    if (!tcp_headers(p, &ip, &th)) return orig_output(n, p, dst);

    uint16_t sport = lwip_ntohs(th->src);
    if (sport != RELAY_HTTP_PORT && sport != RELAY_TLS_PORT)
        return orig_output(n, p, dst);

    uint32_t orig_ip = 0;
    uint16_t orig_port = 0;
    bool found = false;
    taskENTER_CRITICAL(&flow_mux);
    flow_t *f = flow_find(lwip_ntohs(th->dest));
    if (f && f->relay_port == sport) {
        orig_ip = f->orig_ip;
        orig_port = f->orig_port;
        if (f->state == FLOW_ACTIVE) f->stamp_ms = now_ms();
        found = true;
    }
    taskEXIT_CRITICAL(&flow_mux);
    if (!found) return orig_output(n, p, dst);

    /* Rewrite a copy: lwIP keeps unacked segments and resends this very
     * pbuf, recomputing the checksum over the header it thinks it wrote. */
    struct pbuf *q = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
    if (q == NULL) return ERR_MEM;
    tcp_headers(q, &ip, &th);
    rewrite_src(ip, th, orig_ip, lwip_htons(orig_port));
    err_t err = orig_output(n, q, dst);
    pbuf_free(q);
    return err;
}

void proxy_attach_netif(struct netif *n) {
    taskENTER_CRITICAL(&flow_mux);
    memset(flows, 0, sizeof(flows));
    taskEXIT_CRITICAL(&flow_mux);
    if (n->output == proxy_netif_output) return;
    orig_output = n->output;
    n->output = proxy_netif_output;
}

// ============================================================
// Request parsing (relay task)
// ============================================================

static const char *find_crlf2(const char *s, size_t n) {
    for (size_t i = 0; i + 3 < n; i++) {
        if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n')
            return s + i;
    }
    return NULL;
}

static bool host_ok(const char *h) {
    if (!*h) return false;
    for (; *h; h++) {
        char c = *h;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
              c == ':' || c == '[' || c == ']'))
            return false;
    }
    return true;
}

static bool out_add(char *out, size_t cap, size_t *w, const char *s, size_t n) {
    if (*w + n > cap) return false;
    memcpy(out + *w, s, n);
    *w += n;
    return true;
}

static bool name_is(const char *s, size_t n, const char *name) {
    return strlen(name) == n && strncasecmp(s, name, n) == 0;
}

/* Turn an origin-form request head into a proxy request. head..head+len is
 * the head including its final empty line. Returns the bytes written to out,
 * or -1. host gets the Host header (or the original IP) for the log. */
static int http_rewrite(const char *head, size_t len, uint32_t orig_ip,
                        char *out, size_t cap, char *host, size_t host_n) {
    const char *end = head + len - 2;       /* the final CRLF */
    const char *le = memchr(head, '\r', len);
    if (!le) return -1;
    const char *sp1 = memchr(head, ' ', le - head);
    if (!sp1) return -1;
    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', le - target);
    if (!sp2 || sp2 == target) return -1;

    /* first pass: Host */
    host[0] = '\0';
    for (const char *s = le + 2; s < end; ) {
        const char *e = memchr(s, '\r', end - s);
        if (!e) e = end;
        const char *c = memchr(s, ':', e - s);
        if (c && name_is(s, c - s, "host")) {
            const char *v = c + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            size_t vn = e - v;
            while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
            if (vn < host_n) {
                memcpy(host, v, vn);
                host[vn] = '\0';
            }
        }
        s = e + 2;
    }
    if (!host_ok(host)) {
        ip4_addr_t a = { .addr = orig_ip };
        ip4addr_ntoa_r(&a, host, host_n);
    }

    size_t w = 0;
    bool ok = out_add(out, cap, &w, head, target - head);
    if (*target == '/') {
        ok = ok && out_add(out, cap, &w, "http://", 7)
                && out_add(out, cap, &w, host, strlen(host));
    }
    ok = ok && out_add(out, cap, &w, target, le + 2 - target);

    /* second pass: copy headers, minus the connection-management ones */
    for (const char *s = le + 2; ok && s < end; ) {
        const char *e = memchr(s, '\r', end - s);
        if (!e) e = end;
        const char *c = memchr(s, ':', e - s);
        size_t nn = c ? (size_t)(c - s) : 0;
        bool drop = c && (name_is(s, nn, "connection") ||
                          name_is(s, nn, "proxy-connection") ||
                          name_is(s, nn, "keep-alive"));
        if (!drop) ok = out_add(out, cap, &w, s, e + 2 - s);
        s = e + 2;
    }
    /* One request per connection: a second request on a kept-alive socket
     * would reach the proxy unrewritten. */
    static const char tail[] =
        "Connection: close\r\nProxy-Connection: close\r\n\r\n";
    ok = ok && out_add(out, cap, &w, tail, sizeof(tail) - 1);
    return ok ? (int)w : -1;
}

/* SNI from a TLS ClientHello record. false if absent or malformed. */
static bool tls_sni(const uint8_t *b, size_t n, char *out, size_t on) {
    if (n < 5 || b[0] != 0x16) return false;
    size_t end = 5 + (((size_t)b[3] << 8) | b[4]);
    if (end > n) end = n;
    size_t p = 5;
    if (p + 4 > end || b[p] != 0x01) return false;     /* ClientHello */
    p += 4 + 2 + 32;                                    /* header, version, random */
    if (p + 1 > end) return false;
    p += 1 + b[p];                                      /* session id */
    if (p + 2 > end) return false;
    p += 2 + (((size_t)b[p] << 8) | b[p + 1]);          /* cipher suites */
    if (p + 1 > end) return false;
    p += 1 + b[p];                                      /* compression */
    if (p + 2 > end) return false;
    size_t ext_end = p + 2 + (((size_t)b[p] << 8) | b[p + 1]);
    p += 2;
    if (ext_end > end) ext_end = end;
    while (p + 4 <= ext_end) {
        uint16_t type = ((uint16_t)b[p] << 8) | b[p + 1];
        size_t len = ((size_t)b[p + 2] << 8) | b[p + 3];
        p += 4;
        if (p + len > ext_end) return false;
        if (type == 0) {                                /* server_name */
            size_t q = p + 2, qe = p + len;
            while (q + 3 <= qe) {
                uint8_t nt = b[q];
                size_t nl = ((size_t)b[q + 1] << 8) | b[q + 2];
                q += 3;
                if (q + nl > qe) return false;
                if (nt == 0 && nl > 0 && nl < on) {
                    memcpy(out, b + q, nl);
                    out[nl] = '\0';
                    return host_ok(out);
                }
                q += nl;
            }
            return false;
        }
        p += len;
    }
    return false;
}

// ============================================================
// Upstream proxy address (relay task)
// ============================================================

static char resolved_host[PROXY_HOST_MAX];
static struct in_addr resolved_addr;
static bool resolved_valid = false;

static bool proxy_addr(struct sockaddr_in *sa) {
    bool on;
    char host[PROXY_HOST_MAX];
    uint16_t port;
    proxy_get(&on, host, sizeof(host), &port);
    if (!host[0] || !port) return false;

    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1) return true;

    if (resolved_valid && strcmp(resolved_host, host) == 0) {
        sa->sin_addr = resolved_addr;
        return true;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        ESP_LOGW(TAG, "can't resolve proxy host %s", host);
        return false;
    }
    resolved_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    strlcpy(resolved_host, host, sizeof(resolved_host));
    resolved_valid = true;
    sa->sin_addr = resolved_addr;
    return true;
}

// ============================================================
// Relay sessions (relay task)
// ============================================================

typedef enum {
    S_HEAD,         /* reading the request head / ClientHello from the phone */
    S_CONNECTING,   /* non-blocking connect to the proxy */
    S_REPLY,        /* HTTPS: waiting for the proxy's answer to CONNECT */
    S_SPLICE,       /* copying both ways */
} sess_state_t;

typedef struct {
    bool used, tls;
    sess_state_t st;
    int ph, px;                     /* phone and proxy sockets */
    uint16_t phone_port;
    uint32_t orig_ip;               /* network order */
    uint8_t *mem;                   /* up (UP_CAP) + down (DOWN_SZ) */
    uint8_t *up;   size_t up_len, up_off;
    uint8_t *down; size_t down_len, down_off;
    bool ph_eof, px_eof, px_shut;
    uint32_t deadline;
    char host[80];
} sess_t;

static sess_t sess[SESS_MAX];

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static bool would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

static void sess_close(sess_t *s, const char *why) {
    if (why && debug_log_on(DBG_VERBOSE)) {
        ESP_LOGI(TAG, "[%u] %s %s: %s", s->phone_port,
                 s->tls ? "https" : "http", s->host, why);
    }
    if (s->ph >= 0) close(s->ph);
    if (s->px >= 0) close(s->px);
    free(s->mem);
    flow_mark_closed(s->phone_port);
    memset(s, 0, sizeof(*s));
    s->ph = s->px = -1;
    active_sessions--;
}

static void sess_fail(sess_t *s, const char *why) {
    ESP_LOGW(TAG, "[%u] %s %s: %s", s->phone_port,
             s->tls ? "https" : "http", s->host, why);
    sess_close(s, NULL);
}

static void on_connected(sess_t *s, uint32_t now) {
    if (s->tls) {
        char req[200];
        int n = snprintf(req, sizeof(req),
            "CONNECT %s:443 HTTP/1.1\r\nHost: %s:443\r\n\r\n", s->host, s->host);
        if (n <= 0 || n >= (int)sizeof(req) || send(s->px, req, n, 0) != n) {
            sess_fail(s, "sending CONNECT failed");
            return;
        }
        s->st = S_REPLY;
        s->down_len = 0;
        s->deadline = now + SETUP_MS;
    } else {
        s->st = S_SPLICE;
        s->up_off = 0;
        s->deadline = now + IDLE_MS;
    }
}

static void start_connect(sess_t *s, uint32_t now) {
    struct sockaddr_in sa;
    if (!proxy_addr(&sa)) {
        sess_fail(s, "no proxy address");
        return;
    }
    s->px = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->px < 0) {
        sess_fail(s, "out of sockets");
        return;
    }
    set_nonblock(s->px);
    if (debug_log_on(DBG_VERBOSE)) {
        ESP_LOGI(TAG, "[%u] %s %s via proxy", s->phone_port,
                 s->tls ? "CONNECT" : "GET", s->host);
    }
    if (connect(s->px, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        on_connected(s, now);
    } else if (errno == EINPROGRESS) {
        s->st = S_CONNECTING;
        s->deadline = now + SETUP_MS;
    } else {
        resolved_valid = false;
        sess_fail(s, "connecting to the proxy failed");
    }
}

/* Phone data in S_HEAD: enough to know where it is going? */
static void head_ready(sess_t *s, uint32_t now) {
    if (s->tls) {
        if (s->up_len < 5) return;
        size_t rec = 5 + (((size_t)s->up[3] << 8) | s->up[4]);
        if (s->up[0] == 0x16 && s->up_len < rec && s->up_len < UP_SZ) return;
        if (!tls_sni(s->up, s->up_len, s->host, sizeof(s->host))) {
            ip4_addr_t a = { .addr = s->orig_ip };
            ip4addr_ntoa_r(&a, s->host, sizeof(s->host));
        }
        start_connect(s, now);
        return;
    }

    const char *e = find_crlf2((const char *)s->up, s->up_len);
    if (!e) {
        if (s->up_len >= UP_SZ) sess_fail(s, "request head too large");
        return;
    }
    size_t head = e + 4 - (const char *)s->up;
    size_t body = s->up_len - head;
    char *out = malloc(UP_CAP);
    if (!out) {
        sess_fail(s, "out of memory");
        return;
    }
    int n = http_rewrite((const char *)s->up, head, s->orig_ip,
                         out, UP_CAP - body, s->host, sizeof(s->host));
    if (n < 0) {
        free(out);
        sess_fail(s, "can't parse the request");
        return;
    }
    memcpy(out + n, s->up + head, body);   /* start of a POST body, if any */
    memcpy(s->up, out, n + body);
    s->up_len = n + body;
    free(out);
    start_connect(s, now);
}

static void reply_ready(sess_t *s, uint32_t now) {
    s->down[s->down_len] = '\0';
    const char *e = find_crlf2((const char *)s->down, s->down_len);
    if (!e) {
        if (s->down_len >= DOWN_SZ - 1) sess_fail(s, "proxy reply too large");
        return;
    }
    const char *d = (const char *)s->down;
    if (strncmp(d, "HTTP/1.", 7) != 0 || s->down_len < 12 || d[9] != '2') {
        char line[48];
        size_t ln = strcspn(d, "\r\n");
        if (ln >= sizeof(line)) ln = sizeof(line) - 1;
        memcpy(line, d, ln);
        line[ln] = '\0';
        ESP_LOGW(TAG, "[%u] proxy refused CONNECT %s: %s",
                 s->phone_port, s->host, line);
        sess_close(s, NULL);
        return;
    }
    size_t head = e + 4 - d;
    s->down_len -= head;
    memmove(s->down, s->down + head, s->down_len);
    s->down_off = 0;
    s->up_off = 0;              /* the ClientHello goes out first */
    s->st = S_SPLICE;
    s->deadline = now + IDLE_MS;
}

/* false if the session was closed */
static bool splice(sess_t *s, fd_set *rd, fd_set *wr, uint32_t now) {
    bool moved = false;

    if (s->up_off < s->up_len && FD_ISSET(s->px, wr)) {
        int n = send(s->px, s->up + s->up_off, s->up_len - s->up_off, 0);
        if (n < 0 && !would_block()) { sess_close(s, "proxy write error"); return false; }
        if (n > 0) { s->up_off += n; moved = true; }
        if (s->up_off == s->up_len) s->up_off = s->up_len = 0;
    } else if (s->up_len == 0 && !s->ph_eof && FD_ISSET(s->ph, rd)) {
        int n = recv(s->ph, s->up, UP_SZ, 0);
        if (n < 0 && !would_block()) { sess_close(s, "phone reset"); return false; }
        if (n == 0) s->ph_eof = true;
        if (n > 0) { s->up_len = n; s->up_off = 0; moved = true; }
    }

    if (s->down_off < s->down_len && FD_ISSET(s->ph, wr)) {
        int n = send(s->ph, s->down + s->down_off, s->down_len - s->down_off, 0);
        if (n < 0 && !would_block()) { sess_close(s, "phone write error"); return false; }
        if (n > 0) { s->down_off += n; moved = true; }
        if (s->down_off == s->down_len) s->down_off = s->down_len = 0;
    } else if (s->down_len == 0 && !s->px_eof && FD_ISSET(s->px, rd)) {
        int n = recv(s->px, s->down, DOWN_SZ, 0);
        if (n < 0 && !would_block()) { sess_close(s, "proxy reset"); return false; }
        if (n == 0) s->px_eof = true;
        if (n > 0) { s->down_len = n; s->down_off = 0; moved = true; }
    }

    if (s->ph_eof && s->up_len == 0 && !s->px_shut) {
        shutdown(s->px, SHUT_WR);
        s->px_shut = true;
    }
    if (s->px_eof && s->down_len == 0) { sess_close(s, "done"); return false; }
    if (moved) s->deadline = now + IDLE_MS;
    return true;
}

static void sess_service(sess_t *s, fd_set *rd, fd_set *wr, uint32_t now) {
    switch (s->st) {
        case S_HEAD:
            if (FD_ISSET(s->ph, rd)) {
                int n = recv(s->ph, s->up + s->up_len, UP_SZ - s->up_len, 0);
                if (n == 0 || (n < 0 && !would_block())) {
                    sess_close(s, "phone closed before sending");
                    return;
                }
                if (n > 0) {
                    s->up_len += n;
                    head_ready(s, now);
                }
            }
            break;
        case S_CONNECTING:
            if (FD_ISSET(s->px, wr)) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s->px, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0) {
                    resolved_valid = false;
                    sess_fail(s, "connecting to the proxy failed");
                    return;
                }
                on_connected(s, now);
            }
            break;
        case S_REPLY:
            if (FD_ISSET(s->px, rd)) {
                int n = recv(s->px, s->down + s->down_len,
                             DOWN_SZ - 1 - s->down_len, 0);
                if (n == 0 || (n < 0 && !would_block())) {
                    sess_fail(s, "proxy closed after CONNECT");
                    return;
                }
                if (n > 0) {
                    s->down_len += n;
                    reply_ready(s, now);
                }
            }
            break;
        case S_SPLICE:
            splice(s, rd, wr, now);
            break;
    }
}

static void sess_fds(sess_t *s, fd_set *rd, fd_set *wr, int *maxfd) {
#define ADD(fd, set) do { FD_SET((fd), (set)); if ((fd) > *maxfd) *maxfd = (fd); } while (0)
    switch (s->st) {
        case S_HEAD:       ADD(s->ph, rd); break;
        case S_CONNECTING: ADD(s->px, wr); break;
        case S_REPLY:      ADD(s->px, rd); break;
        case S_SPLICE:
            if (s->up_off < s->up_len) ADD(s->px, wr);
            else if (!s->ph_eof)       ADD(s->ph, rd);
            if (s->down_off < s->down_len) ADD(s->ph, wr);
            else if (!s->px_eof)           ADD(s->px, rd);
            break;
    }
#undef ADD
}

static void do_accept(int lsock, bool tls, uint32_t now) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int c = accept(lsock, (struct sockaddr *)&peer, &plen);
    if (c < 0) return;

    uint16_t pport = ntohs(peer.sin_port);
    uint32_t orig_ip;
    uint16_t orig_port;
    /* Only connections the input hook redirected: anything else (a stray
     * connection from the WiFi side, say) would make this an open relay. */
    if (peer.sin_addr.s_addr != PHONE_ADDR_N ||
        !flow_lookup(pport, tls ? RELAY_TLS_PORT : RELAY_HTTP_PORT,
                     &orig_ip, &orig_port)) {
        close(c);
        return;
    }

    sess_t *s = NULL;
    for (int i = 0; i < SESS_MAX; i++) {
        if (!sess[i].used) { s = &sess[i]; break; }
    }
    uint8_t *mem = s ? malloc(UP_CAP + DOWN_SZ + 1) : NULL;
    if (!mem) {
        ESP_LOGW(TAG, "[%u] out of memory for a session, dropped", pport);
        close(c);
        flow_mark_closed(pport);
        return;
    }
    set_nonblock(c);
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->tls = tls;
    s->st = S_HEAD;
    s->ph = c;
    s->px = -1;
    s->phone_port = pport;
    s->orig_ip = orig_ip;
    s->mem = mem;
    s->up = mem;
    s->down = mem + UP_CAP;
    s->deadline = now + SETUP_MS;
    ip4_addr_t a = { .addr = orig_ip };
    ip4addr_ntoa_r(&a, s->host, sizeof(s->host));
    active_sessions++;
}

static int make_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void proxy_task(void *arg) {
    (void)arg;
    for (int i = 0; i < SESS_MAX; i++) sess[i].ph = sess[i].px = -1;

    int lh = make_listener(RELAY_HTTP_PORT);
    int lt = make_listener(RELAY_TLS_PORT);
    if (lh < 0 || lt < 0) {
        ESP_LOGE(TAG, "relay listeners failed, proxy unavailable");
        if (lh >= 0) close(lh);
        if (lt >= 0) close(lt);
        taskENTER_CRITICAL(&state_mux);
        px_ready = false;
        px_on = false;
        taskEXIT_CRITICAL(&state_mux);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        int maxfd = -1;
        bool room = false;
        for (int i = 0; i < SESS_MAX; i++) {
            if (sess[i].used) sess_fds(&sess[i], &rd, &wr, &maxfd);
            else room = true;
        }
        /* With every slot busy, or the heap low, new connections wait in the
         * backlog: accepting only to close them would fail the page. */
        if (room && heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < MIN_FREE_HEAP)
            room = false;
        if (room) {
            FD_SET(lh, &rd);
            FD_SET(lt, &rd);
            if (lh > maxfd) maxfd = lh;
            if (lt > maxfd) maxfd = lt;
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(maxfd + 1, &rd, &wr, NULL, &tv);
        uint32_t now = now_ms();
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (r > 0) {
            for (int i = 0; i < SESS_MAX; i++) {
                if (sess[i].used) sess_service(&sess[i], &rd, &wr, now);
            }
            if (room && FD_ISSET(lh, &rd)) do_accept(lh, false, now);
            if (room && FD_ISSET(lt, &rd)) do_accept(lt, true, now);
        }
        for (int i = 0; i < SESS_MAX; i++) {
            sess_t *s = &sess[i];
            if (s->used && (int32_t)(now - s->deadline) > 0) {
                if (s->st == S_SPLICE) sess_close(s, "idle");
                else sess_fail(s, s->st == S_HEAD ? "timed out waiting for the request" :
                                  s->st == S_CONNECTING ? "timed out connecting to the proxy" :
                                  "timed out waiting for the CONNECT reply");
            }
        }
    }
}

void proxy_init(void) {
    settings_load();
    bool on;
    char host[PROXY_HOST_MAX];
    uint16_t port;
    proxy_get(&on, host, sizeof(host), &port);
    if (on) ESP_LOGI(TAG, "HTTP proxy on, %s:%u", host, (unsigned)port);
    else    ESP_LOGI(TAG, "HTTP proxy off");
    gw_safe_task_create(proxy_task, "proxy", 4096, NULL, 5, NULL);
}
