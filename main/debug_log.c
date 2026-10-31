/*
 * debug_log.c — optional extended logging, switched from the web UI (/debug).
 *
 * Three independent switches, kept as one bitmask in NVS so a trace can cover
 * boot and pairing, not just what happens after someone ticks a box:
 *
 *   DBG_HCI_PACKETS  every HCI packet, hex-dumped by BTstack's hci_dump
 *   DBG_BTSTACK_LOG  BTstack's own log_info/log_error lines ("LOG -- ...")
 *   DBG_VERBOSE      extra bridge-side detail (see debug_log_on() callers)
 *
 * All output goes to the USB serial console only. hci_dump is always
 * initialised so the switches can flip at runtime; with everything off it
 * costs one flag check per packet. The trace prints from inside the BTstack
 * run loop over a 115200-baud UART, so with packets on, PPP throughput drops
 * hard. It is a diagnostic tool, not something to leave on.
 *
 * Threading: hci_dump's flags are plain globals read on the BTstack task, so
 * changes from the web UI are applied there via the run loop, the same way
 * bt_name.c handles renames.
 */

#include "btstack_config.h"
#include "btstack.h"
#include "hci_dump.h"
#include "hci_dump_embedded_stdout.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "gateway.h"

static const char *TAG = "debug_log";

#define NVS_NAMESPACE   "gw"        /* same namespace wifi_multi.c uses */
#define NVS_KEY_FLAGS   "dbgflags"

static volatile uint8_t dbg_flags = 0;
static volatile bool apply_pending = false;

static void nvs_flags_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_FLAGS, &v) == ESP_OK) {
        dbg_flags = v & DBG_ALL;
    }
    nvs_close(h);
}

static void nvs_flags_save(uint8_t flags) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (flags == 0) {
        nvs_erase_key(h, NVS_KEY_FLAGS);    /* absent = all off */
    } else {
        nvs_set_u8(h, NVS_KEY_FLAGS, flags);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* BTstack task */
static void apply_to_hci_dump(void) {
    uint8_t f = dbg_flags;
    bool stack_log = (f & DBG_BTSTACK_LOG) != 0;
    hci_dump_enable_packet_log((f & DBG_HCI_PACKETS) != 0);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_DEBUG, stack_log);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_INFO,  stack_log);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_ERROR, stack_log);
}

/* Run-loop trampoline for changes coming from another task (the web UI). */
static void apply_cb(void *context) {
    (void)context;
    taskENTER_CRITICAL(&state_mux);
    apply_pending = false;
    taskEXIT_CRITICAL(&state_mux);
    apply_to_hci_dump();
}

static btstack_context_callback_registration_t apply_cb_reg = {
    .callback = apply_cb,
    .context  = NULL,
};

// ============================================================
// API
// ============================================================

void debug_log_init(void) {
    nvs_flags_load();
    if (dbg_flags != 0) {
        ESP_LOGW(TAG, "extended logging on:%s%s%s",
                 (dbg_flags & DBG_HCI_PACKETS) ? " hci-packets" : "",
                 (dbg_flags & DBG_BTSTACK_LOG) ? " btstack-log" : "",
                 (dbg_flags & DBG_VERBOSE)     ? " verbose"     : "");
    }
}

void debug_log_bt_setup(void) {
    hci_dump_init(hci_dump_embedded_stdout_get_instance());
    apply_to_hci_dump();    /* hci_dump_init defaults everything to on */
}

uint8_t debug_log_get(void) {
    return dbg_flags;
}

bool debug_log_on(uint8_t flag) {
    return (dbg_flags & flag) != 0;
}

void debug_log_set(uint8_t flags) {
    flags &= DBG_ALL;
    taskENTER_CRITICAL(&state_mux);
    dbg_flags = flags;
    bool already = apply_pending;
    if (!already) apply_pending = true;
    taskEXIT_CRITICAL(&state_mux);

    nvs_flags_save(flags);
    ESP_LOGI(TAG, "extended logging set to 0x%02x", flags);

    if (!already) {
        btstack_run_loop_execute_on_main_thread(&apply_cb_reg);
    }
}
