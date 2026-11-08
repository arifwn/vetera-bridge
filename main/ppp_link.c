/*
 * ppp_link.c — RFCOMM server ↔ lwIP PPPoS server glue.
 *
 * The phone (gnubox-modified Symbian) opens an RFCOMM channel (LAP ch 3 or
 * SPP ch 4) and immediately speaks raw PPP over it. We terminate that PPP
 * with lwIP's PPPoS in server mode and NAT the resulting netif to WiFi.
 *
 * Threading (see gateway.h):
 *  - rfcomm_* only from the BTstack task. pppapi_* calls block on the tcpip
 *    task, so they are made from the BTstack task — never from tcpip itself.
 *  - RX: RFCOMM data → pppos_input_tcpip() (copies + posts, thread-safe).
 *  - TX: pppos output cb (tcpip task) → stream buffer → run-loop trampoline
 *    → rfcomm_request_can_send_now_event → rfcomm_send in chunks.
 */

#include <string.h>
#include <strings.h>

#include "btstack_config.h"
#include "btstack.h"

#include "lwip/tcpip.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "netif/ppp/pppapi.h"

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#include "gateway.h"

static const char *TAG = "ppp_link";

#define LAP_RFCOMM_CHANNEL   3
#define SPP_RFCOMM_CHANNEL   4

/* Worst-case HDLC-escaped 1500-byte PPP frame is ~3010 bytes; 8 KB holds a
 * couple of those (or ~5 typical frames) across RFCOMM credit stalls. */
#define PPP_TX_BUF_SIZE      8192
#define PPP_TX_CHUNK_MAX     1024

/* --- state (shared fields under state_mux, see gateway.h) --- */

static StreamBufferHandle_t ppp_tx_stream;      /* tcpip → BTstack bytes    */
static volatile bool tx_notify_pending = false; /* trampoline queued        */
static volatile uint32_t tx_dropped = 0;

static uint16_t active_cid = 0;                 /* under state_mux          */
static volatile bool session_active = false;    /* RFCOMM channel open      */
static volatile bool ppp_up_flag    = false;    /* IPCP negotiated          */
static volatile bool ppp_teardown   = false;    /* we initiated ppp close   */

/* ppp pcb: created and closed only from the BTstack task; freed by the
 * status callback (tcpip task) during the synchronous pppapi_close(). */
static ppp_pcb *ppp = NULL;
static struct netif ppp_netif;

static uint8_t tx_chunk[PPP_TX_CHUNK_MAX];

/* --- pre-PPP handshake (BTstack task only) ---
 * gnubox redirects the phone's GSM-data bearer to the BT serial port, but
 * the phone still runs its dial-up script against that "modem" before it
 * starts PPP: Symbian's direct-connect CSY does the Microsoft DCC dance
 * (sends "CLIENT", expects "CLIENTSERVER" back); other configurations
 * talk AT commands and expect "CONNECT" to the dial. The canonical Linux
 * gnubox server answers via chat 'CLIENT' 'CLIENTSERVER' — without a
 * reply the phone never sends its first LCP frame and the link hangs at
 * 0 bytes. Answer both dialects, then hand off to PPP for good at the
 * first HDLC flag (0x7E). */
static bool ppp_frame_seen = false;
static const char *pre_ppp_reply = NULL;

/* --- serial channel security (web UI /security), read once at boot ---
 * NVS: absent = off (the default), 0 = off, 1 = on, 2 = auto. Up to v1.10.0
 * auto was the default and stored as an absent key, so those units come up
 * in off after the upgrade. */
#define NVS_NAMESPACE   "gw"        /* same namespace wifi_multi.c uses */
#define NVS_KEY_SECURE  "btsec"

static btsec_mode_t link_mode = BTSEC_OFF;

static btsec_mode_t nvs_secure_load(void) {
    nvs_handle_t h;
    uint8_t v = BTSEC_OFF;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return BTSEC_OFF;
    nvs_get_u8(h, NVS_KEY_SECURE, &v);
    nvs_close(h);
    if (v == BTSEC_ON) return BTSEC_ON;
    if (v == BTSEC_AUTO) return BTSEC_AUTO;
    return BTSEC_OFF;
}

btsec_mode_t ppp_link_secure_get(void) {
    return link_mode;
}

btsec_mode_t ppp_link_secure_saved(void) {
    return nvs_secure_load();
}

void ppp_link_secure_save(btsec_mode_t mode) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SECURE, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "serial channel security saved as %s, applies on reboot",
             btsec_mode_str(mode));
}

const char *btsec_mode_str(btsec_mode_t mode) {
    switch (mode) {
        case BTSEC_OFF: return "off";
        case BTSEC_ON:  return "on";
        default:        return "auto";
    }
}

/* --- auto mode (BTstack task only) ---
 * S60 1st Edition phones connect without security and the N-Gage QD fails
 * with it; S60 2nd Edition phones fail without it (see ppp_link_init). In auto
 * the L2CAP level stays at 0 and the RFCOMM accept is held while the bridge
 * asks the phone for its name: a factory name from the list below (S60 1st
 * Edition, Series 80 Communicators) is accepted as is, any other name (or none)
 * gets authentication + encryption first. A phone renamed away from its model
 * number therefore counts as S60v2. */
static const char *const nosec_names[] = {
    "N-Gage", "NGage",                  /* classic and QD */
    "3650", "3600",                     /* 3600: the 3650's Americas model */
    "3660", "3620",                     /* 3620: the 3660's Americas model */
    "7650",
    "9210", "9290",                     /* Series 80: 9210/9210i, 9290 its Americas model */
    "9300", "9500",                     /* 9300 also matches the 9300i */
};

static uint16_t auto_cid = 0;           /* RFCOMM channel held, 0 = none */
static hci_con_handle_t auto_handle;
static bd_addr_t auto_addr;
static bool auto_sec_pending = false;   /* waiting for GAP_EVENT_SECURITY_LEVEL */
static btstack_packet_callback_registration_t auto_hci_cb;

/* Last auto decision, for the /security page. Under state_mux. */
static char last_phone_name[BT_NAME_MAX];
static bool last_phone_secured = false;
static bool last_phone_valid = false;

bool ppp_link_last_phone(char *name, int name_len, bool *secured) {
    bool valid;
    taskENTER_CRITICAL(&state_mux);
    valid = last_phone_valid;
    if (valid) {
        strlcpy(name, last_phone_name, name_len);
        *secured = last_phone_secured;
    }
    taskEXIT_CRITICAL(&state_mux);
    return valid;
}

static bool contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) return true;
    }
    return false;
}

static bool is_nosec_name(const char *name) {
    for (size_t i = 0; i < sizeof(nosec_names) / sizeof(nosec_names[0]); i++) {
        if (contains_nocase(name, nosec_names[i])) return true;
    }
    return false;
}

static void auto_release(void) {
    auto_cid = 0;
    auto_sec_pending = false;
}

static void auto_fail(const char *why) {
    ESP_LOGW(TAG, "auto security: %s, declining the phone", why);
    uint16_t cid = auto_cid;
    auto_release();
    rfcomm_decline_connection(cid);
    taskENTER_CRITICAL(&state_mux);
    if (active_cid == cid) {
        session_active = false;
        active_cid = 0;
    }
    taskEXIT_CRITICAL(&state_mux);
}

/* name: NULL when the phone didn't answer the name request. */
static void auto_decide(const char *name) {
    bool secure = name == NULL || !is_nosec_name(name);
    ESP_LOGI(TAG, "auto security: phone '%s' -> %s",
             name ? name : "(no name)",
             secure ? "authentication + encryption" : "known model, no security");

    taskENTER_CRITICAL(&state_mux);
    strlcpy(last_phone_name, name ? name : "", sizeof(last_phone_name));
    last_phone_secured = secure;
    last_phone_valid = true;
    taskEXIT_CRITICAL(&state_mux);

    if (!secure) {
        uint16_t cid = auto_cid;
        auto_release();
        rfcomm_accept_connection(cid);
        return;
    }
    auto_sec_pending = true;
    gap_request_security_level(auto_handle, LEVEL_2);
}

static void auto_begin(uint16_t cid, hci_con_handle_t handle,
                       const bd_addr_t addr) {
    auto_cid = cid;
    auto_handle = handle;
    bd_addr_copy(auto_addr, addr);
    auto_sec_pending = false;
    /* The ACL is already up, so the controller asks over it and ignores
     * the page scan mode (R1) and clock offset. */
    if (gap_remote_name_request(addr, 1, 0) != ERROR_CODE_SUCCESS) {
        auto_decide(NULL);
    }
}

static void auto_hci_handler(uint8_t packet_type, uint16_t channel,
                             uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET || auto_cid == 0) return;

    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            if (auto_sec_pending) break;
            bd_addr_t addr;
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            if (bd_addr_cmp(addr, auto_addr) != 0) break;
            if (hci_event_remote_name_request_complete_get_status(packet) != 0) {
                auto_decide(NULL);
                break;
            }
            /* up to 248 bytes, NUL-terminated only when shorter */
            const char *rn =
                hci_event_remote_name_request_complete_get_remote_name(packet);
            char name[BT_NAME_MAX];
            size_t n = strnlen(rn, sizeof(name) - 1);
            memcpy(name, rn, n);
            name[n] = '\0';
            auto_decide(name);
            break;
        }
        case GAP_EVENT_SECURITY_LEVEL: {
            if (!auto_sec_pending) break;
            if (gap_event_security_level_get_handle(packet) != auto_handle) break;
            uint8_t status = gap_event_security_level_get_status(packet);
            gap_security_level_t level = (gap_security_level_t)
                gap_event_security_level_get_security_level(packet);
            if (status != 0 || level < LEVEL_2) {
                auto_fail("authentication or encryption failed");
                break;
            }
            uint16_t cid = auto_cid;
            auto_release();
            rfcomm_accept_connection(cid);
            break;
        }
        default:
            break;
    }
}

// ============================================================
// NAT
// ============================================================

static void update_nat_lwip_ctx(void *arg) {
    (void)arg;
    static bool napt_on = false;
    bool enable = ppp_up_flag && get_wifi_connected();
    if (enable == napt_on) return;
    ip_napt_enable_netif(&ppp_netif, enable ? 1 : 0);
    napt_on = enable;
    ESP_LOGI(TAG, "NAPT %s", enable ? "enabled" : "disabled");
}

void update_nat(void) {
    /* tcpip_callback only posts — safe from any task incl. tcpip itself */
    tcpip_callback(update_nat_lwip_ctx, NULL);
}

bool ppp_link_up(void) {
    return ppp_up_flag;
}

struct netif *ppp_link_netif(void) {
    return &ppp_netif;
}

uint32_t ppp_link_tx_dropped(void) {
    return tx_dropped;
}

// ============================================================
// TX path: pppos output (tcpip task) → BTstack task
// ============================================================

static void tx_notify_cb(void *context) {  /* runs in BTstack task */
    (void)context;
    uint16_t cid;
    taskENTER_CRITICAL(&state_mux);
    tx_notify_pending = false;
    cid = active_cid;
    taskEXIT_CRITICAL(&state_mux);
    if (cid) rfcomm_request_can_send_now_event(cid);
}

static btstack_context_callback_registration_t tx_cb_reg = {
    .callback = tx_notify_cb,
    .context  = NULL,
};

static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
    (void)pcb; (void)ctx;
    if (!session_active) return len;    /* link gone: swallow */

    size_t sent = xStreamBufferSend(ppp_tx_stream, data, len, 0);
    if (sent < len) tx_dropped++;       /* partial frame -> peer FCS drops it */

    /* Re-enqueueing a queued registration corrupts the run loop list,
     * hence the pending flag. */
    bool need;
    taskENTER_CRITICAL(&state_mux);
    need = !tx_notify_pending;
    if (need) tx_notify_pending = true;
    taskEXIT_CRITICAL(&state_mux);
    if (need) btstack_run_loop_execute_on_main_thread(&tx_cb_reg);
    return len;
}

// ============================================================
// PPP session
// ============================================================

static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
    (void)ctx;  /* runs in tcpip task */

    if (err_code == PPPERR_NONE) {
        const ip4_addr_t *ip = netif_ip4_addr(&ppp_netif);
        ESP_LOGI(TAG, "PPP up, our %s peer " "192.168.7.%d",
                 ip4addr_ntoa(ip), GW_PEER_IP3);
        ppp_up_flag = true;
        update_nat();
        gw_on_ppp_up();
        return;
    }

    ESP_LOGW(TAG, "PPP down, err %d", err_code);
    ppp_up_flag = false;
    update_nat();
    gw_on_ppp_down();

    if (ppp_teardown) {
        /* Deliberate close from the BTstack task (inside pppapi_close).
         * We are in the tcpip context: raw ppp_free is correct here,
         * pppapi_free would deadlock. */
        ppp_free(pcb);
    } else if (session_active) {
        /* PPP died but the RFCOMM channel is still open (e.g. the phone
         * terminated LCP). Re-arm so a redial on the same channel works. */
        ESP_LOGI(TAG, "re-listening for PPP on open RFCOMM channel");
        ppp_listen(pcb);
    }
}

/* BTstack task */
static bool ppp_session_start(void) {
    ppp = pppapi_pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, NULL);
    if (ppp == NULL) {
        ESP_LOGE(TAG, "pppos_create failed");
        return false;
    }
    /* netif_add just set ppp_netif.output; wrap it while the link is still
     * silent, so the proxy redirect can restore reply source addresses. */
    proxy_attach_netif(&ppp_netif);

    ip4_addr_t our, his, dns;
    IP4_ADDR(&our, GW_IP0, GW_IP1, GW_IP2, GW_IP3);
    IP4_ADDR(&his, GW_IP0, GW_IP1, GW_IP2, GW_PEER_IP3);
    IP4_ADDR(&dns, GW_IP0, GW_IP1, GW_IP2, GW_IP3);
    ppp_set_ipcp_ouraddr(ppp, &our);
    ppp_set_ipcp_hisaddr(ppp, &his);
    ppp_set_ipcp_dnsaddr(ppp, 0, &dns);

    /* Deliberately no auth configuration: PAP/CHAP are compiled out
     * (sdkconfig), so no Authentication-Protocol option ever hits the
     * wire — old Symbian PPP stacks crash on auth options they don't
     * understand. Do not "fix" this by calling ppp_set_auth_required:
     * it does not exist without PPP_AUTH_SUPPORT. */

    /* Wait silently for the phone's first LCP ConfReq. */
    ppp_set_silent(ppp, 1);

    err_t err = pppapi_listen(ppp);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "pppapi_listen failed: %d", (int)err);
        ppp_teardown = true;
        pppapi_close(ppp, 1);
        ppp_teardown = false;
        ppp = NULL;
        return false;
    }
    ESP_LOGI(TAG, "PPP server listening");
    return true;
}

/* BTstack task */
static void ppp_session_stop(void) {
    taskENTER_CRITICAL(&state_mux);
    session_active = false;
    active_cid = 0;
    taskEXIT_CRITICAL(&state_mux);

    if (ppp != NULL) {
        ppp_teardown = true;
        pppapi_close(ppp, 1);   /* synchronous; status cb frees the pcb */
        ppp_teardown = false;
        ppp = NULL;
    }
    ppp_up_flag = false;
}

// ============================================================
// RFCOMM packet handler (BTstack task)
// ============================================================

/* Returns true if the data was consumed by the handshake (not PPP). */
static bool pre_ppp_handshake(const uint8_t *pkt, uint16_t size, uint16_t cid) {
    if (ppp_frame_seen) return false;
    if (memchr(pkt, 0x7E, size) != NULL) {
        ppp_frame_seen = true;
        ESP_LOGI(TAG, "first PPP flag seen — handing link off to PPP");
        return false;               /* feed this packet to PPP too */
    }

    char txt[41];
    uint16_t n = size < (uint16_t)(sizeof(txt) - 1) ? size
                                                    : (uint16_t)(sizeof(txt) - 1);
    for (uint16_t i = 0; i < n; i++)
        txt[i] = (pkt[i] >= 0x20 && pkt[i] < 0x7F) ? (char)pkt[i] : '.';
    txt[n] = '\0';
    ESP_LOGI(TAG, "pre-PPP RX (%u bytes): '%s'", size, txt);

    if (strstr(txt, "CLIENT") != NULL) {
        pre_ppp_reply = "CLIENTSERVER";
    } else if (strstr(txt, "ATD") != NULL) {
        pre_ppp_reply = "\r\nCONNECT 115200\r\n";
    } else if (strstr(txt, "AT") != NULL) {
        pre_ppp_reply = "\r\nOK\r\n";
    }
    if (pre_ppp_reply != NULL) rfcomm_request_can_send_now_event(cid);
    return true;
}

static void rfcomm_packet_handler(uint8_t packet_type, uint16_t channel,
                                   uint8_t *packet, uint16_t size) {
    bd_addr_t addr;
    uint16_t cid;

    if (packet_type == RFCOMM_DATA_PACKET) {
        bool ok;
        taskENTER_CRITICAL(&state_mux);
        ok = session_active && (channel == active_cid);
        taskEXIT_CRITICAL(&state_mux);
        if (ok && ppp != NULL) {
            if (pre_ppp_handshake(packet, size, channel)) return;
            pppos_input_tcpip(ppp, packet, size);
        }
        return;
    }

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

        case RFCOMM_EVENT_INCOMING_CONNECTION: {
            cid = rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
            rfcomm_event_incoming_connection_get_bd_addr(packet, addr);
            uint8_t server_ch =
                rfcomm_event_incoming_connection_get_server_channel(packet);

            bool busy;
            taskENTER_CRITICAL(&state_mux);
            busy = session_active;
            if (!busy) {
                session_active = true;
                active_cid = cid;
            }
            taskEXIT_CRITICAL(&state_mux);

            if (busy) {
                ESP_LOGW(TAG, "incoming RFCOMM (ch %u) from %s while busy — declined",
                         server_ch, bd_addr_to_str(addr));
                rfcomm_decline_connection(cid);
            } else if (link_mode == BTSEC_AUTO) {
                ESP_LOGI(TAG, "incoming RFCOMM ch %u from %s — asking its name",
                         server_ch, bd_addr_to_str(addr));
                auto_begin(cid,
                           rfcomm_event_incoming_connection_get_con_handle(packet),
                           addr);
            } else {
                ESP_LOGI(TAG, "incoming RFCOMM ch %u from %s — accepting",
                         server_ch, bd_addr_to_str(addr));
                rfcomm_accept_connection(cid);
            }
            break;
        }

        case RFCOMM_EVENT_CHANNEL_OPENED: {
            uint8_t status = rfcomm_event_channel_opened_get_status(packet);
            cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            if (status != 0) {
                ESP_LOGW(TAG, "RFCOMM open failed, status 0x%02x", status);
                if (cid == auto_cid) auto_release();
                taskENTER_CRITICAL(&state_mux);
                if (active_cid == cid || active_cid == 0) {
                    session_active = false;
                    active_cid = 0;
                }
                taskEXIT_CRITICAL(&state_mux);
                break;
            }

            uint16_t mfs = rfcomm_event_channel_opened_get_max_frame_size(packet);
            uint16_t con_handle = rfcomm_event_channel_opened_get_con_handle(packet);
            ESP_LOGI(TAG, "RFCOMM channel opened, cid 0x%04x, max frame %u",
                     cid, mfs);
            if (debug_log_on(DBG_VERBOSE)) {
                /* For legacy PIN-paired phones gap_authenticated() reads 0
                 * even on an authenticated link: trust level and key_size. */
                ESP_LOGI(TAG, "link security: level=%d authenticated=%d "
                         "key_size=%u",
                         (int)gap_security_level(con_handle),
                         (int)gap_authenticated(con_handle),
                         (unsigned)gap_encryption_key_size(con_handle));
            }

            xStreamBufferReset(ppp_tx_stream);
            taskENTER_CRITICAL(&state_mux);
            tx_notify_pending = false;
            taskEXIT_CRITICAL(&state_mux);
            tx_dropped = 0;
            ppp_frame_seen = false;
            pre_ppp_reply = NULL;

            if (!ppp_session_start()) {
                rfcomm_disconnect(cid);
                taskENTER_CRITICAL(&state_mux);
                session_active = false;
                active_cid = 0;
                taskEXIT_CRITICAL(&state_mux);
                break;
            }
            gw_on_rfcomm_opened(con_handle);
            break;
        }

        case RFCOMM_EVENT_CAN_SEND_NOW: {
            uint16_t send_cid;
            taskENTER_CRITICAL(&state_mux);
            send_cid = active_cid;
            taskEXIT_CRITICAL(&state_mux);
            if (send_cid == 0) break;

            if (pre_ppp_reply != NULL) {
                uint8_t st = rfcomm_send(send_cid, (uint8_t *)pre_ppp_reply,
                                         (uint16_t)strlen(pre_ppp_reply));
                if (st == 0) {
                    ESP_LOGI(TAG, "pre-PPP TX: replied to handshake");
                    pre_ppp_reply = NULL;
                } else {
                    rfcomm_request_can_send_now_event(send_cid);
                }
                break;
            }

            uint16_t mfs = rfcomm_get_max_frame_size(send_cid);
            if (mfs == 0 || mfs > sizeof(tx_chunk)) mfs = sizeof(tx_chunk);

            size_t n = xStreamBufferReceive(ppp_tx_stream, tx_chunk, mfs, 0);
            if (n > 0) {
                uint8_t st = rfcomm_send(send_cid, tx_chunk, (uint16_t)n);
                if (st != 0) {
                    /* bytes already consumed from the stream — the phone's
                     * PPP FCS will drop the mangled frame, TCP retransmits */
                    tx_dropped++;
                    ESP_LOGW(TAG, "rfcomm_send failed: 0x%02x", st);
                }
            }
            if (xStreamBufferBytesAvailable(ppp_tx_stream) > 0) {
                rfcomm_request_can_send_now_event(send_cid);
            }
            break;
        }

        case RFCOMM_EVENT_CHANNEL_CLOSED: {
            cid = rfcomm_event_channel_closed_get_rfcomm_cid(packet);
            bool ours;
            taskENTER_CRITICAL(&state_mux);
            ours = (cid == active_cid);
            taskEXIT_CRITICAL(&state_mux);
            if (!ours) break;

            if (cid == auto_cid) {
                /* the phone gave up while auto mode held the accept */
                ESP_LOGI(TAG, "RFCOMM channel closed before it was accepted");
                auto_release();
                taskENTER_CRITICAL(&state_mux);
                session_active = false;
                active_cid = 0;
                taskEXIT_CRITICAL(&state_mux);
                break;
            }

            ESP_LOGI(TAG, "RFCOMM channel closed");
            ppp_session_stop();
            gw_on_rfcomm_closed();
            break;
        }

        default:
            break;
    }
}

// ============================================================
// Init (BTstack task, before hci_power_control)
// ============================================================

void ppp_link_init(void) {
    ppp_tx_stream = xStreamBufferCreate(PPP_TX_BUF_SIZE, 1);
    if (ppp_tx_stream == NULL) {
        ESP_LOGE(TAG, "failed to allocate TX stream buffer");
        abort();
    }

    l2cap_init();
    rfcomm_init();
    sdp_init();

    /* Optionally require an authenticated, encrypted link before RFCOMM
     * accepts a phone. Symbian's BTCOMM client on S60v2 does not start
     * security itself: with a security requirement set (gnubox always sets
     * one), it expects the server to have authenticated and encrypted the
     * link and otherwise fails every read and write with -6005. This is what
     * "Secure Connections" / "Enable Data Encryption" did on Widcomm and
     * BlueSoleil. S60v1 phones have always connected without it, and the
     * N-Gage QD fails with it, so the default is auto: decided per phone by
     * its name (see auto_decide). BTstack takes the level when the first
     * RFCOMM service registers, so a change needs a reboot. SDP stays open
     * either way. */
    link_mode = nvs_secure_load();
    rfcomm_set_required_security_level(link_mode == BTSEC_ON ? LEVEL_2 : LEVEL_0);
    if (link_mode == BTSEC_AUTO) {
        auto_hci_cb.callback = &auto_hci_handler;
        hci_add_event_handler(&auto_hci_cb);
    }
    ESP_LOGI(TAG, "serial channel security: %s",
             link_mode == BTSEC_ON  ? "authentication + encryption required" :
             link_mode == BTSEC_OFF ? "off" : "auto (by phone name)");

    /* Automatic credit management: RX drains into a much faster WiFi
     * uplink, so no manual rfcomm_grant_credits throttling is needed. */
    rfcomm_register_service(rfcomm_packet_handler, LAP_RFCOMM_CHANNEL, 0xffff);
    rfcomm_register_service(rfcomm_packet_handler, SPP_RFCOMM_CHANNEL, 0xffff);

    sdp_register_lap_and_spp(LAP_RFCOMM_CHANNEL, SPP_RFCOMM_CHANNEL);
}
