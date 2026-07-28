/*
 * gateway.c — application state machine, GAP/pairing, watchdog, bring-up.
 * Structure follows satura-bridge's pan_wifi_bridge.c.
 */

#include <string.h>
#include <inttypes.h>

#include "btstack_config.h"
#include "btstack.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "gateway.h"

static const char *TAG = "gateway";

#define BT_REOPEN_DELAY_MS      1200
#define HEAP_WARN_THRESHOLD     24576   /* 24 KB — warn early */
#define HEAP_REBOOT_THRESHOLD   8192    /* 8 KB — reboot before allocator panics */
#define HEARTBEAT_INTERVAL_MS   30000
#define WIFI_KICK_DELAY_MS      3000    /* let IPCP settle before the scan */

/* Class of Device: service class Networking, major class LAN Access Point.
 * The phone's device picker may filter on CoD — look like a LAN AP. */
#define BT_CLASS_OF_DEVICE      0x020300

portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile app_state_t app_state = APP_WAIT_BT;
static volatile bool bt_connected = false;
static volatile int8_t bt_rssi = -100;
static hci_con_handle_t bt_handle = HCI_CON_HANDLE_INVALID;
static int64_t boot_us = 0;

/* One-at-a-time guards (under state_mux) */
static volatile bool bt_reopen_running  = false;
static volatile bool wifi_kick_running  = false;

static btstack_packet_callback_registration_t hci_event_cb;

// ============================================================
// Helpers
// ============================================================

void gw_safe_task_create(TaskFunction_t fn, const char *name,
                          uint32_t stack, void *arg,
                          UBaseType_t prio, TaskHandle_t *handle) {
    if (xTaskCreate(fn, name, stack, arg, prio, handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task %s, rebooting...", name);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

uint32_t uptime_seconds(void) {
    return (uint32_t)((esp_timer_get_time() - boot_us) / 1000000ULL);
}

const char *state_to_str(app_state_t st) {
    switch (st) {
        case APP_WAIT_BT:         return "WAIT_BT";
        case APP_NO_WIFI:         return "NO_WIFI";
        case APP_WIFI_SCANNING:   return "WIFI_SCAN";
        case APP_WIFI_CONNECTING: return "WIFI_CONN";
        case APP_WIFI_FAILED:     return "WIFI_FAIL";
        case APP_BRIDGE:          return "BRIDGE_ACTIVE";
        case APP_BRIDGE_NO_WIFI:  return "BRIDGE_LOST_WIFI";
        default:                  return "UNKNOWN";
    }
}

void set_app_state(app_state_t new_state) {
    app_state_t old_state;
    bool changed = false;
    taskENTER_CRITICAL(&state_mux);
    old_state = app_state;
    if (old_state != new_state) {
        app_state = new_state;
        changed = true;
    }
    taskEXIT_CRITICAL(&state_mux);
    if (changed)
        ESP_LOGI(TAG, "[STATE] %s -> %s",
                 state_to_str(old_state), state_to_str(new_state));
}

app_state_t get_app_state(void) {
    app_state_t st;
    taskENTER_CRITICAL(&state_mux);
    st = app_state;
    taskEXIT_CRITICAL(&state_mux);
    return st;
}

bool get_bt_connected(void) {
    bool v;
    taskENTER_CRITICAL(&state_mux);
    v = bt_connected;
    taskEXIT_CRITICAL(&state_mux);
    return v;
}

int8_t get_bt_rssi(void) {
    int8_t v;
    taskENTER_CRITICAL(&state_mux);
    v = bt_rssi;
    taskEXIT_CRITICAL(&state_mux);
    return v;
}

static hci_con_handle_t get_bt_handle(void) {
    hci_con_handle_t h;
    taskENTER_CRITICAL(&state_mux);
    h = bt_handle;
    taskEXIT_CRITICAL(&state_mux);
    return h;
}

// ============================================================
// BT visibility
// ============================================================

void bt_set_visible(bool v) {
    gap_discoverable_control(v ? 1 : 0);
    gap_connectable_control(v ? 1 : 0);
    ESP_LOGI(TAG, "[BT] %s", v ? "visible" : "hidden");
}

static void bt_reopen_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BT_REOPEN_DELAY_MS));
    if (!get_bt_connected()) bt_set_visible(true);
    taskENTER_CRITICAL(&state_mux);
    bt_reopen_running = false;
    taskEXIT_CRITICAL(&state_mux);
    vTaskDelete(NULL);
}

// ============================================================
// PPP / RFCOMM lifecycle hooks (called from ppp_link.c)
// ============================================================

void gw_on_rfcomm_opened(uint16_t con_handle) {
    taskENTER_CRITICAL(&state_mux);
    bt_connected = true;
    bt_handle    = (hci_con_handle_t)con_handle;
    taskEXIT_CRITICAL(&state_mux);
    bt_set_visible(false);
    /* WiFi is kicked from gw_on_ppp_up, after IPCP settles */
}

void gw_on_rfcomm_closed(void) {
    taskENTER_CRITICAL(&state_mux);
    bt_connected = false;
    bt_handle    = HCI_CON_HANDLE_INVALID;
    bt_rssi      = -100;
    bool already = bt_reopen_running;
    if (!already) bt_reopen_running = true;
    taskEXIT_CRITICAL(&state_mux);

    set_app_state(APP_WAIT_BT);
    update_nat();

    if (!already) {
        gw_safe_task_create(bt_reopen_task, "btr", 4096, NULL, 4, NULL);
    }
}

/* Guarded delay task: wait for IPCP/coex to settle, then start WiFi. */
static void wifi_kick_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_KICK_DELAY_MS));
    taskENTER_CRITICAL(&state_mux);
    wifi_kick_running = false;
    taskEXIT_CRITICAL(&state_mux);
    if (ppp_link_up() && !get_wifi_connected() && wifi_list_count() > 0) {
        wifi_connect_begin();
    }
    vTaskDelete(NULL);
}

/* Deferred, guarded WiFi kick — the scan monopolizes the shared radio for
 * seconds, so anything that must still cross the BT link first (IPCP, an
 * in-flight HTTP response) gets WIFI_KICK_DELAY_MS to finish. Safe from
 * any task; the kick task re-checks conditions after the delay. */
void gw_wifi_kick_deferred(void) {
    bool already;
    taskENTER_CRITICAL(&state_mux);
    already = wifi_kick_running;
    if (!already) wifi_kick_running = true;
    taskEXIT_CRITICAL(&state_mux);
    if (!already) {
        gw_safe_task_create(wifi_kick_task, "wkick", 3072, NULL, 5, NULL);
    }
}

/* tcpip task context — only flags, state and task spawns here */
void gw_on_ppp_up(void) {
    if (get_wifi_connected()) {
        set_app_state(APP_BRIDGE);
        return;
    }
    if (wifi_list_count() == 0) {
        set_app_state(APP_NO_WIFI);
        return;
    }
    gw_wifi_kick_deferred();
}

void gw_on_ppp_down(void) {
    app_state_t st = get_app_state();
    if (st == APP_BRIDGE || st == APP_BRIDGE_NO_WIFI) {
        set_app_state(APP_WAIT_BT);
    }
}

// ============================================================
// HCI packet handler (pairing, logging, RSSI)
// ============================================================

static void hci_packet_handler(uint8_t type, uint16_t ch,
                                uint8_t *pkt, uint16_t sz) {
    (void)ch; (void)sz;
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;
    switch (hci_event_packet_get_type(pkt)) {
        case BTSTACK_EVENT_STATE:
            /* The controller only reports its BD_ADDR once it is up, and the
             * device name is derived from it — compose it now. */
            if (btstack_event_state_get_state(pkt) == HCI_STATE_WORKING) {
                bt_name_apply_from_addr();
            }
            break;
        case HCI_EVENT_CONNECTION_REQUEST:
            hci_event_connection_request_get_bd_addr(pkt, addr);
            ESP_LOGI(TAG, "[HCI] Connection request from %s",
                     bd_addr_to_str(addr));
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            hci_event_connection_complete_get_bd_addr(pkt, addr);
            ESP_LOGI(TAG, "[HCI] Connection complete status=0x%02x addr=%s",
                     hci_event_connection_complete_get_status(pkt),
                     bd_addr_to_str(addr));
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            ESP_LOGI(TAG, "[HCI] Disconnected handle=0x%04x reason=0x%02x",
                hci_event_disconnection_complete_get_connection_handle(pkt),
                hci_event_disconnection_complete_get_reason(pkt));
            break;
        case GAP_EVENT_RSSI_MEASUREMENT:
            if (gap_event_rssi_measurement_get_con_handle(pkt) == get_bt_handle()) {
                taskENTER_CRITICAL(&state_mux);
                bt_rssi = gap_event_rssi_measurement_get_rssi(pkt);
                taskEXIT_CRITICAL(&state_mux);
            }
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            /* N-Gage era = legacy pairing with a PIN, entered on the phone */
            hci_event_pin_code_request_get_bd_addr(pkt, addr);
            ESP_LOGI(TAG, "[HCI] PIN request from %s — answering '%s'",
                     bd_addr_to_str(addr), BT_LEGACY_PIN);
            gap_pin_code_response(addr, BT_LEGACY_PIN);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(pkt, addr);
            gap_ssp_confirmation_response(addr);
            break;
        case HCI_EVENT_LINK_KEY_REQUEST:
            hci_event_link_key_request_get_bd_addr(pkt, addr);
            ESP_LOGI(TAG, "[HCI] Link key request from %s", bd_addr_to_str(addr));
            break;
        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            ESP_LOGI(TAG, "[HCI] Link key notification");
            break;
        case GAP_EVENT_PAIRING_COMPLETE: {
            uint8_t pstatus = gap_event_pairing_complete_get_status(pkt);
            gap_event_pairing_complete_get_bd_addr(pkt, addr);
            ESP_LOGI(TAG, "[GAP] Pairing complete status=0x%02x addr=%s",
                     pstatus, bd_addr_to_str(addr));
            if (pstatus == 0) bt_bootstrap_on_pairing_complete(addr);
            break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            ESP_LOGI(TAG, "[HCI] Authentication complete status=0x%02x handle=0x%04x",
                     hci_event_authentication_complete_get_status(pkt),
                     hci_event_authentication_complete_get_connection_handle(pkt));
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            ESP_LOGI(TAG, "[HCI] Encryption change status=0x%02x enabled=%d handle=0x%04x",
                     hci_event_encryption_change_get_status(pkt),
                     hci_event_encryption_change_get_encryption_enabled(pkt),
                     hci_event_encryption_change_get_connection_handle(pkt));
            break;
        case HCI_EVENT_COMMAND_STATUS: {
            uint8_t st = hci_event_command_status_get_status(pkt);
            if (st != 0) {
                ESP_LOGW(TAG, "[HCI] Command status error=0x%02x opcode=0x%04x",
                         st, hci_event_command_status_get_command_opcode(pkt));
            }
            break;
        }
        case HCI_EVENT_COMMAND_COMPLETE:
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
            /* Routine/high-frequency — would flood the log for no diagnostic
             * value (command_complete fires after every controller command,
             * e.g. every RSSI poll). */
            break;
        default:
            ESP_LOGI(TAG, "[HCI] event 0x%02x", hci_event_packet_get_type(pkt));
            break;
    }
}

// ============================================================
// Watchdog & heartbeat
// ============================================================

/* gap_read_rssi must run in the BTstack task */
static void rssi_poll_cb(void *context) {
    (void)context;
    hci_con_handle_t h = get_bt_handle();
    if (h != HCI_CON_HANDLE_INVALID) gap_read_rssi(h);
}

static btstack_context_callback_registration_t rssi_cb_reg = {
    .callback = rssi_poll_cb,
    .context  = NULL,
};

static void watchdog_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));

        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        size_t min_heap  = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

        app_state_t st = get_app_state();
        bool b_conn = get_bt_connected();
        bool w_conn = get_wifi_connected();

        btstack_run_loop_execute_on_main_thread(&rssi_cb_reg);

        uint32_t up = uptime_seconds();
        ESP_LOGI(TAG,
            "[HB] State:%s | BT:%s RSSI:%d | PPP:%s txdrop:%" PRIu32
            " | WiFi:%s RSSI:%d | Heap:%dKB min:%dKB | Up:%" PRIu32
            "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
            state_to_str(st),
            b_conn ? "ON" : "OFF", (int)get_bt_rssi(),
            ppp_link_up() ? "UP" : "DOWN", ppp_link_tx_dropped(),
            w_conn ? "ON" : "OFF", (int)wifi_get_rssi(),
            (int)(free_heap / 1024), (int)(min_heap / 1024),
            up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60);

        wifi_watchdog_check();
        dns_fwd_watchdog_check();

        if (free_heap < HEAP_WARN_THRESHOLD) {
            ESP_LOGW(TAG, "[WDT] Low heap: %d bytes", (int)free_heap);
        }
        if (free_heap < HEAP_REBOOT_THRESHOLD) {
            ESP_LOGE(TAG, "[WDT] Critical heap (%d), rebooting...",
                     (int)free_heap);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

// ============================================================
// Bring-up
// ============================================================

static void bt_setup(void) {
    /* Placeholder until BTSTACK_EVENT_STATE composes the real name — BTstack
     * keeps this pointer and re-reads it on every name write. */
    gap_set_local_name(bt_name_active_ptr());
    gap_discoverable_control(1);
    gap_connectable_control(1);
    gap_set_class_of_device(BT_CLASS_OF_DEVICE);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_set_security_level(LEVEL_0);

    hci_event_cb.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_cb);

#if defined(L2CAP_SET_MAX_MTU)
    l2cap_set_max_mtu(1691);
#endif

    ppp_link_init();
}

int btstack_main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    boot_us = esp_timer_get_time();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash error %d, erasing...", ret);
        nvs_flash_erase();
        nvs_flash_init();
    }

    bt_name_init();
    wifi_multi_init();
    dns_fwd_start();
    gw_safe_task_create(watchdog_task, "wdt", 4096, NULL, 2, NULL);
    web_ui_start();
    bt_setup();
    hci_power_control(HCI_POWER_ON);

    /* The device name isn't known until the controller reports its address —
     * bt_name.c logs it from the BTSTACK_EVENT_STATE handler. */
    ESP_LOGI(TAG, "Vetera Bridge up — pair from the phone (PIN %s), "
             "then dial via gnubox", BT_LEGACY_PIN);
    return 0;
}
