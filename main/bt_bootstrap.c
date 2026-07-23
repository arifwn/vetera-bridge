/*
 * bt_bootstrap.c — one-shot "mRouter registration" bootstrap.
 *
 * Some gnubox builds never store the ESP32 as the phone's default BT comm
 * port unless something has connected *into* the phone's own Bluetooth
 * serial service at least once (the mRouter theory — see README and
 * PLANNED_UPDATE.md). After a successful pairing we therefore:
 *
 *   1. SDP-probe the phone (browse root) and log every RFCOMM service it
 *      exposes — the investigation step the whole plan depends on.
 *   2. Query specifically for a Serial Port (0x1101) class service.
 *   3. If found: rfcomm_create_channel() into it, hold the link briefly,
 *      then disconnect. No data is sent or read — the act of connecting
 *      is the point, mimicking what a PC serial connection does.
 *
 * Runs once per newly-paired address (RAM-tracked, not persisted; a fresh
 * pairing is a rare, deliberate user action). No retries, no persistent
 * client state. ppp_link.c's server-side data path is untouched: the
 * bootstrap channel uses its own packet handler, so the two never mix.
 *
 * Threading: GAP events, SDP client callbacks, RFCOMM events and run-loop
 * timers all fire in the BTstack task — no locking needed here.
 */

#include <string.h>

#include "btstack_config.h"
#include "btstack.h"

#include "esp_log.h"

#include "gateway.h"

static const char *TAG = "bt_boot";

#define BOOTSTRAP_START_DELAY_MS  1500  /* let post-pairing traffic settle */
#define BOOTSTRAP_HOLD_MS         3000  /* how long to hold the link open  */
#define BOOTSTRAP_MAX_DONE        4     /* matches NVM_NUM_LINK_KEYS       */

typedef enum {
    BOOT_IDLE = 0,
    BOOT_WAIT_START,    /* delay timer running                  */
    BOOT_PROBE,         /* browse-root SDP query, log everything */
    BOOT_TARGET,        /* 0x1101 query to pick the channel      */
    BOOT_CONNECT,       /* outgoing rfcomm_create_channel in flight */
    BOOT_HOLD,          /* channel open, hold timer running      */
} boot_state_t;

static boot_state_t boot_state = BOOT_IDLE;
static bd_addr_t    boot_addr;
static uint8_t      target_channel = 0;
static uint16_t     boot_cid = 0;
static btstack_timer_source_t boot_timer;

/* addresses already bootstrapped this boot (attempted counts as done) */
static bd_addr_t done_addrs[BOOTSTRAP_MAX_DONE];
static int       done_count = 0;

/* drop_acl: BTstack never drops an idle ACL on its own, so every path that
 * may have paged the phone (SDP query or RFCOMM connect) must drop the
 * baseband link on the way out — otherwise the phone shows "connected"
 * forever and the shared radio wastes time on a dead link. Pass false only
 * when the ACL belongs to someone else (the phone's own live session). */
static void boot_reset(bool drop_acl) {
    btstack_run_loop_remove_timer(&boot_timer);
    if (drop_acl) {
        hci_connection_t *con =
            hci_connection_for_bd_addr_and_type(boot_addr, BD_ADDR_TYPE_ACL);
        if (con != NULL) gap_disconnect(con->con_handle);
    }
    boot_state = BOOT_IDLE;
    target_channel = 0;
    boot_cid = 0;
}

static bool addr_already_done(const bd_addr_t addr) {
    for (int i = 0; i < done_count; i++) {
        if (bd_addr_cmp(done_addrs[i], addr) == 0) return true;
    }
    return false;
}

// ============================================================
// Outgoing RFCOMM channel (BTstack task)
// ============================================================

static void boot_rfcomm_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;

    if (packet_type == RFCOMM_DATA_PACKET) {
        /* likely the phone's AT/modem interface talking — ignore */
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

        case RFCOMM_EVENT_CHANNEL_OPENED: {
            uint8_t status = rfcomm_event_channel_opened_get_status(packet);
            if (status != 0) {
                ESP_LOGW(TAG, "connect into phone ch %u failed, status 0x%02x "
                         "— bootstrap abandoned (no retry)",
                         target_channel, status);
                boot_reset(true);
                break;
            }
            boot_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            ESP_LOGI(TAG, "connected into phone's serial service (ch %u) — "
                     "holding %d ms to register as its BT comm port",
                     target_channel, BOOTSTRAP_HOLD_MS);
            boot_state = BOOT_HOLD;
            btstack_run_loop_set_timer(&boot_timer, BOOTSTRAP_HOLD_MS);
            btstack_run_loop_add_timer(&boot_timer);
            break;
        }

        case RFCOMM_EVENT_CHANNEL_CLOSED:
            ESP_LOGI(TAG, "bootstrap link closed — done. Retry the gnubox "
                     "dial now (2box Direct -> Bluetooth -> %s)",
                     BT_LOCAL_NAME);
            boot_reset(true);
            break;

        default:
            break;
    }
}

// ============================================================
// SDP queries (BTstack task)
// ============================================================

static void boot_sdp_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)packet_type; (void)channel; (void)size;

    switch (hci_event_packet_get_type(packet)) {

        case SDP_EVENT_QUERY_RFCOMM_SERVICE: {
            uint8_t ch = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
            const char *name = sdp_event_query_rfcomm_service_get_name(packet);
            ESP_LOGI(TAG, "[SDP] phone service: ch %u name '%s'%s",
                     ch, name,
                     boot_state == BOOT_TARGET ? " (serial-port class)" : "");
            if (boot_state == BOOT_TARGET && target_channel == 0 && ch != 0) {
                target_channel = ch;
            }
            break;
        }

        case SDP_EVENT_QUERY_COMPLETE: {
            uint8_t status = sdp_event_query_complete_get_status(packet);

            if (boot_state == BOOT_PROBE) {
                if (status != 0) {
                    ESP_LOGW(TAG, "SDP probe failed, status 0x%02x — "
                             "bootstrap abandoned", status);
                    boot_reset(true);
                    break;
                }
                /* probe logged the full inventory; now pick by class */
                boot_state = BOOT_TARGET;
                uint8_t err = sdp_client_query_rfcomm_channel_and_name_for_service_class_uuid(
                        boot_sdp_handler, boot_addr,
                        BLUETOOTH_SERVICE_CLASS_SERIAL_PORT);
                if (err != 0) {
                    ESP_LOGW(TAG, "serial-port SDP query didn't start (0x%02x)", err);
                    boot_reset(true);
                }
                break;
            }

            /* BOOT_TARGET */
            if (target_channel == 0) {
                ESP_LOGW(TAG, "phone exposes no serial-port (0x1101) service "
                         "— nothing to bootstrap into (status 0x%02x)", status);
                boot_reset(true);
                break;
            }
            ESP_LOGI(TAG, "connecting into phone %s ch %u (one-shot)",
                     bd_addr_to_str(boot_addr), target_channel);
            boot_state = BOOT_CONNECT;
            uint8_t err = rfcomm_create_channel(boot_rfcomm_handler, boot_addr,
                                                target_channel, &boot_cid);
            if (err != 0) {
                ESP_LOGW(TAG, "rfcomm_create_channel failed (0x%02x)", err);
                boot_reset(true);
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================
// Timers (start delay + hold)
// ============================================================

static void boot_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    switch (boot_state) {

        case BOOT_WAIT_START: {
            if (get_bt_connected()) {
                /* a PPP session is live — don't disturb it; the whole point
                 * is moot anyway, the phone clearly managed to dial in */
                ESP_LOGI(TAG, "RFCOMM session active — bootstrap skipped");
                boot_reset(false);   /* that ACL is the phone's session */
                break;
            }
            ESP_LOGI(TAG, "SDP-probing %s for RFCOMM services...",
                     bd_addr_to_str(boot_addr));
            boot_state = BOOT_PROBE;
            uint8_t err = sdp_client_query_rfcomm_channel_and_name_for_uuid(
                    boot_sdp_handler, boot_addr,
                    BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
            if (err != 0) {
                ESP_LOGW(TAG, "SDP probe didn't start (0x%02x)", err);
                boot_reset(true);
            }
            break;
        }

        case BOOT_HOLD:
            ESP_LOGI(TAG, "hold done — disconnecting bootstrap link");
            rfcomm_disconnect(boot_cid);
            /* CHANNEL_CLOSED finishes the state machine */
            break;

        default:
            break;
    }
}

// ============================================================
// Trigger (called from gateway.c's HCI handler, BTstack task)
// ============================================================

void bt_bootstrap_on_pairing_complete(const uint8_t addr[6]) {
    if (boot_state != BOOT_IDLE) {
        ESP_LOGI(TAG, "bootstrap already in progress — skipped");
        return;
    }
    if (addr_already_done(addr)) {
        ESP_LOGI(TAG, "%s already bootstrapped this boot — skipped",
                 bd_addr_to_str(addr));
        return;
    }
    /* mark now: attempted counts as done, one-shot means no retries */
    if (done_count < BOOTSTRAP_MAX_DONE) {
        memcpy(done_addrs[done_count++], addr, sizeof(bd_addr_t));
    }
    memcpy(boot_addr, addr, sizeof(bd_addr_t));
    boot_state = BOOT_WAIT_START;

    ESP_LOGI(TAG, "new pairing — scheduling mRouter bootstrap into %s in %d ms",
             bd_addr_to_str(boot_addr), BOOTSTRAP_START_DELAY_MS);
    btstack_run_loop_set_timer_handler(&boot_timer, boot_timer_handler);
    btstack_run_loop_set_timer(&boot_timer, BOOTSTRAP_START_DELAY_MS);
    btstack_run_loop_add_timer(&boot_timer);
}
