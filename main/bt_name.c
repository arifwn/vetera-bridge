/*
 * bt_name.c — the Bluetooth device name shown in the phone's device list.
 *
 * With more than one bridge powered on at once, a fixed name makes the units
 * indistinguishable at pairing time — and since the bridge auto-answers the PIN
 * at LEVEL_0, picking the wrong one *succeeds*. So every unit names itself:
 *
 *   - default: BT_NAME_PREFIX + a decimal number derived from the low two bytes
 *     of the BD_ADDR, which is the per-chip part of the ESP32 MAC.
 *   - override: a friendly name set from the web UI (/name), kept in NVS.
 *
 * The address is only known once the controller is up, so the name is composed
 * on BTSTACK_EVENT_STATE / HCI_STATE_WORKING (see gateway.c) rather than at
 * bt_setup() time. gap_set_local_name() may be called again at any point — it
 * re-schedules both the local-name write and the EIR refresh.
 *
 * Threading: name_active is the buffer BTstack keeps a pointer to; it builds the
 * HCI command from it later, from the BTstack task, so only the BTstack task
 * writes it. Readers (web UI, log lines) copy it out under state_mux.
 */

#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "gateway.h"

static const char *TAG = "bt_name";

#define NVS_NAMESPACE   "gw"        /* same namespace wifi_multi.c uses */
#define NVS_KEY_NAME    "btname"

/* The one buffer handed to gap_set_local_name — static lifetime, BTstack task
 * writes only. Holds the prefix until the controller reports its address. */
static char name_active[BT_NAME_MAX] = BT_NAME_PREFIX;

/* The NVS-backed override, empty when unset. Under state_mux. */
static char name_custom[BT_NAME_MAX] = {0};

/* The address-derived name, cached so readers in other tasks don't have to
 * call into BTstack for it. Empty until the controller is up. Under state_mux. */
static char name_derived[BT_NAME_MAX] = {0};

/* Set while an apply is queued on the BTstack task: re-adding the same
 * callback registration while it is still in the run loop's list would corrupt
 * that list. Under state_mux. */
static volatile bool apply_pending = false;

// ============================================================
// NVS
// ============================================================

static void nvs_name_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    char tmp[BT_NAME_MAX] = {0};
    size_t len = sizeof(tmp);
    bool ok = nvs_get_str(h, NVS_KEY_NAME, tmp, &len) == ESP_OK;
    nvs_close(h);
    if (!ok) return;
    tmp[sizeof(tmp) - 1] = '\0';
    taskENTER_CRITICAL(&state_mux);
    strlcpy(name_custom, tmp, sizeof(name_custom));
    taskEXIT_CRITICAL(&state_mux);
}

static void nvs_name_save(const char *name) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (name[0] == '\0') {
        nvs_erase_key(h, NVS_KEY_NAME);     /* absent = use the derived name */
    } else {
        nvs_set_str(h, NVS_KEY_NAME, name);
    }
    nvs_commit(h);
    nvs_close(h);
}

// ============================================================
// Name composition (BTstack task)
// ============================================================

/* Copy src into name_active. Called from the BTstack task only; the critical
 * section is there to pair with bt_name_get's read. */
static void set_active(const char *src) {
    taskENTER_CRITICAL(&state_mux);
    strlcpy(name_active, src, sizeof(name_active));
    taskEXIT_CRITICAL(&state_mux);
}

void bt_name_apply_from_addr(void) {
    char custom[BT_NAME_MAX];
    taskENTER_CRITICAL(&state_mux);
    strlcpy(custom, name_custom, sizeof(custom));
    taskEXIT_CRITICAL(&state_mux);

    bd_addr_t addr;
    gap_local_bd_addr(addr);

    char derived[BT_NAME_MAX];
    snprintf(derived, sizeof(derived), BT_NAME_PREFIX " %u",
             (unsigned)((addr[4] << 8) | addr[5]));

    taskENTER_CRITICAL(&state_mux);
    strlcpy(name_derived, derived, sizeof(name_derived));
    taskEXIT_CRITICAL(&state_mux);

    set_active(custom[0] != '\0' ? custom : derived);
    gap_set_local_name(name_active);
    ESP_LOGI(TAG, "device name '%s' (addr %s)", name_active,
             bd_addr_to_str(addr));
}

/* Run-loop trampoline for renames coming from another task (the web UI). */
static void apply_cb(void *context) {
    (void)context;
    taskENTER_CRITICAL(&state_mux);
    apply_pending = false;
    taskEXIT_CRITICAL(&state_mux);
    bt_name_apply_from_addr();
}

static btstack_context_callback_registration_t apply_cb_reg = {
    .callback = apply_cb,
    .context  = NULL,
};

// ============================================================
// API
// ============================================================

void bt_name_init(void) {
    nvs_name_load();
    taskENTER_CRITICAL(&state_mux);
    if (name_custom[0] != '\0') {
        /* known already — no need to wait for the controller */
        strlcpy(name_active, name_custom, sizeof(name_active));
    }
    taskEXIT_CRITICAL(&state_mux);
}

const char *bt_name_active_ptr(void) {
    return name_active;
}

void bt_name_get(char *out, int out_len) {
    if (out == NULL || out_len <= 0) return;
    taskENTER_CRITICAL(&state_mux);
    strlcpy(out, name_active, out_len);
    taskEXIT_CRITICAL(&state_mux);
}

void bt_name_get_custom(char *out, int out_len) {
    if (out == NULL || out_len <= 0) return;
    taskENTER_CRITICAL(&state_mux);
    strlcpy(out, name_custom, out_len);
    taskEXIT_CRITICAL(&state_mux);
}

void bt_name_get_derived(char *out, int out_len) {
    if (out == NULL || out_len <= 0) return;
    taskENTER_CRITICAL(&state_mux);
    strlcpy(out, name_derived, out_len);
    taskEXIT_CRITICAL(&state_mux);
}

void bt_name_set(const char *name) {
    /* Printable ASCII only: the name travels in EIR and lands in an S60 device
     * list, and control bytes there are asking for trouble. Leading/trailing
     * blanks are trimmed so an all-spaces entry reads as "clear". */
    char clean[BT_NAME_CUSTOM_MAX + 1] = {0};
    size_t w = 0;
    if (name != NULL) {
        while (*name == ' ') name++;
        for (const char *p = name; *p && w < BT_NAME_CUSTOM_MAX; p++) {
            if (*p < 0x20 || *p > 0x7e) continue;
            clean[w++] = *p;
        }
    }
    while (w > 0 && clean[w - 1] == ' ') clean[--w] = '\0';

    taskENTER_CRITICAL(&state_mux);
    strlcpy(name_custom, clean, sizeof(name_custom));
    bool already = apply_pending;
    if (!already) apply_pending = true;
    taskEXIT_CRITICAL(&state_mux);

    nvs_name_save(clean);
    ESP_LOGI(TAG, "custom name %s", clean[0] ? clean : "cleared");

    /* gap_set_local_name must run on the BTstack task (see gateway.h) */
    if (!already) {
        btstack_run_loop_execute_on_main_thread(&apply_cb_reg);
    }
}
