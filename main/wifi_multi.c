/*
 * wifi_multi.c — WiFi STA with a saved list of networks.
 *
 * Instead of satura-bridge's single SSID, up to WIFI_LIST_MAX networks are
 * stored in NVS (one blob, atomic save). A connect cycle is:
 *   scan → candidates = visible APs ∩ saved list (best RSSI first)
 *        → try each candidate (2 attempts) → all failed = one failed cycle
 *        → exponential backoff (satura's retry machinery) → rescan.
 * After WIFI_MAX_RETRIES failed cycles → APP_WIFI_FAILED, except while the
 * bridge is up (APP_BRIDGE_NO_WIFI) where it retries forever.
 */

#include <string.h>
#include <stdlib.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"

#include "gateway.h"

static const char *TAG = "wifi_multi";

#define NVS_NAMESPACE       "gw"
#define NVS_KEY_LIST        "wifilist"

#define WIFI_MAX_RETRIES    7           /* failed scan+connect cycles */
#define WIFI_RETRY_BASE_MS  1000
#define WIFI_RETRY_MAX_MS   30000
#define CAND_MAX_ATTEMPTS   2           /* connect attempts per candidate */
#define SCAN_MAX_RECORDS    24

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

typedef struct {
    uint8_t ver;                        /* layout version = 1 */
    uint8_t count;
    wifi_cred_t e[WIFI_LIST_MAX];
} wifi_list_t;

/* Shared state — under state_mux unless noted */
static wifi_list_t wifi_list;           /* RAM copy of the NVS blob */
static volatile bool wifi_connected = false;
static volatile bool wifi_ignore_disconnect = false;
static volatile int  wifi_retries = 0;
static volatile uint32_t wifi_retry_delay_ms = WIFI_RETRY_BASE_MS;
static volatile int8_t wifi_rssi = -100;
static char wifi_cur_ssid[33] = {0};    /* SSID we are connected/connecting to */
static char wifi_ip[16] = "--";

/* Candidate cycle state — only touched from the WiFi/event task except for
 * the kick from wifi_connect_begin (guarded by scan_in_progress). */
static volatile bool scan_in_progress = false;
static wifi_cred_t candidates[WIFI_LIST_MAX];
static int8_t cand_rssi[WIFI_LIST_MAX];
static int  cand_count = 0;
static int  cand_index = 0;
static int  cand_attempts = 0;

/* One-at-a-time guards (under state_mux) */
static volatile bool wifi_retry_running    = false;
static volatile bool wifi_recovery_running = false;

static esp_netif_t *sta_netif = NULL;
static esp_event_handler_instance_t wifi_evt_inst = NULL;
static esp_event_handler_instance_t ip_evt_inst   = NULL;

static void wifi_retry_task(void *arg);
static void wifi_recovery_task(void *arg);
static void wifi_soft_reset(void);
static void wifi_schedule_retry(uint32_t delay_ms);
static void wifi_cycle_failed(void);

// ============================================================
// NVS blob
// ============================================================

static bool nvs_list_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    wifi_list_t tmp = {0};
    size_t len = sizeof(tmp);
    bool ok = nvs_get_blob(h, NVS_KEY_LIST, &tmp, &len) == ESP_OK
           && tmp.ver == 1 && tmp.count <= WIFI_LIST_MAX;
    nvs_close(h);
    if (ok) {
        taskENTER_CRITICAL(&state_mux);
        wifi_list = tmp;
        taskEXIT_CRITICAL(&state_mux);
    }
    return ok;
}

static bool nvs_list_save(void) {
    wifi_list_t tmp;
    taskENTER_CRITICAL(&state_mux);
    tmp = wifi_list;
    taskEXIT_CRITICAL(&state_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, NVS_KEY_LIST, &tmp, sizeof(tmp)) == ESP_OK;
    nvs_commit(h);
    nvs_close(h);
    return ok;
}

// ============================================================
// List API (web UI)
// ============================================================

int wifi_list_count(void) {
    int n;
    taskENTER_CRITICAL(&state_mux);
    n = wifi_list.count;
    taskEXIT_CRITICAL(&state_mux);
    return n;
}

bool wifi_list_get_ssid(int index, char *out, int out_len) {
    bool ok = false;
    taskENTER_CRITICAL(&state_mux);
    if (index >= 0 && index < wifi_list.count) {
        strlcpy(out, wifi_list.e[index].ssid, out_len);
        ok = true;
    }
    taskEXIT_CRITICAL(&state_mux);
    return ok;
}

bool wifi_list_add(const char *ssid, const char *pass) {
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > 32) return false;
    if (pass != NULL && strlen(pass) > 64) return false;

    bool ok = false;
    taskENTER_CRITICAL(&state_mux);
    int idx = -1;
    for (int i = 0; i < wifi_list.count; i++) {
        if (strcmp(wifi_list.e[i].ssid, ssid) == 0) { idx = i; break; }
    }
    if (idx < 0 && wifi_list.count < WIFI_LIST_MAX) {
        idx = wifi_list.count++;
    }
    if (idx >= 0) {
        memset(&wifi_list.e[idx], 0, sizeof(wifi_cred_t));
        strlcpy(wifi_list.e[idx].ssid, ssid, sizeof(wifi_list.e[idx].ssid));
        if (pass) strlcpy(wifi_list.e[idx].pass, pass,
                          sizeof(wifi_list.e[idx].pass));
        ok = true;
    }
    taskEXIT_CRITICAL(&state_mux);

    if (ok) nvs_list_save();
    return ok;
}

bool wifi_list_remove(int index) {
    bool ok = false;
    bool was_current = false;
    taskENTER_CRITICAL(&state_mux);
    if (index >= 0 && index < wifi_list.count) {
        was_current = (strcmp(wifi_list.e[index].ssid, wifi_cur_ssid) == 0);
        for (int i = index; i < wifi_list.count - 1; i++) {
            wifi_list.e[i] = wifi_list.e[i + 1];
        }
        wifi_list.count--;
        memset(&wifi_list.e[wifi_list.count], 0, sizeof(wifi_cred_t));
        ok = true;
    }
    taskEXIT_CRITICAL(&state_mux);

    if (ok) {
        nvs_list_save();
        if (was_current && get_wifi_connected()) {
            /* Kicks off a disconnect; the handler reconnect logic will
             * rescan and pick another saved network if one is visible. */
            esp_wifi_disconnect();
        }
    }
    return ok;
}

void wifi_list_clear(void) {
    taskENTER_CRITICAL(&state_mux);
    memset(&wifi_list, 0, sizeof(wifi_list));
    wifi_list.ver = 1;
    memset(wifi_cur_ssid, 0, sizeof(wifi_cur_ssid));
    wifi_ignore_disconnect = true;
    taskEXIT_CRITICAL(&state_mux);
    nvs_list_save();
    esp_wifi_disconnect();
    set_app_state(APP_NO_WIFI);
}

// ============================================================
// Accessors
// ============================================================

bool get_wifi_connected(void) {
    bool v;
    taskENTER_CRITICAL(&state_mux);
    v = wifi_connected;
    taskEXIT_CRITICAL(&state_mux);
    return v;
}

int wifi_get_retries(void) {
    int v;
    taskENTER_CRITICAL(&state_mux);
    v = wifi_retries;
    taskEXIT_CRITICAL(&state_mux);
    return v;
}

int8_t wifi_get_rssi(void) {
    int8_t v;
    taskENTER_CRITICAL(&state_mux);
    v = wifi_rssi;
    taskEXIT_CRITICAL(&state_mux);
    return v;
}

void wifi_get_current(char *ssid_out, int ssid_len, char *ip_out, int ip_len) {
    taskENTER_CRITICAL(&state_mux);
    if (ssid_out) strlcpy(ssid_out, wifi_cur_ssid, ssid_len);
    if (ip_out)   strlcpy(ip_out, wifi_ip, ip_len);
    taskEXIT_CRITICAL(&state_mux);
}

struct esp_netif_obj *wifi_get_sta_netif(void) {
    return (struct esp_netif_obj *)sta_netif;
}

// ============================================================
// Scan-and-pick connect cycle
// ============================================================

/* Start one scan+connect cycle. Callable from httpd / BTstack / timer
 * tasks — esp_wifi APIs are thread-safe. */
void wifi_connect_begin(void) {
    if (wifi_list_count() == 0) {
        set_app_state(APP_NO_WIFI);
        return;
    }
    if (get_wifi_connected() || scan_in_progress) return;

    /* Keep APP_BRIDGE_NO_WIFI visible while re-scanning after a loss —
     * the watchdog's stuck detection and endless-retry policy key off it. */
    if (get_app_state() != APP_BRIDGE_NO_WIFI) {
        set_app_state(APP_WIFI_SCANNING);
    }

    scan_in_progress = true;
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: 0x%x", err);
        scan_in_progress = false;
        wifi_cycle_failed();   /* count it, so backoff still applies */
    }
}

/* One cycle failed (no candidates, or all candidates exhausted). */
static void wifi_cycle_failed(void) {
    app_state_t st = get_app_state();

    taskENTER_CRITICAL(&state_mux);
    int retries = ++wifi_retries;
    uint32_t delay = wifi_retry_delay_ms;
    wifi_retry_delay_ms = (delay * 2 > WIFI_RETRY_MAX_MS)
                          ? WIFI_RETRY_MAX_MS : delay * 2;
    taskEXIT_CRITICAL(&state_mux);

    if (st == APP_BRIDGE_NO_WIFI) {
        /* bridge up, WiFi lost: retry forever */
        wifi_schedule_retry(delay);
    } else if (retries < WIFI_MAX_RETRIES) {
        wifi_schedule_retry(delay);
    } else {
        ESP_LOGW(TAG, "all retry cycles exhausted");
        set_app_state(APP_WIFI_FAILED);
    }
}

static void wifi_try_candidate(void) {
    wifi_cred_t *c = &candidates[cand_index];
    ESP_LOGI(TAG, "trying '%s' (rssi %d, candidate %d/%d, attempt %d)",
             c->ssid, cand_rssi[cand_index], cand_index + 1, cand_count,
             cand_attempts + 1);

    taskENTER_CRITICAL(&state_mux);
    strlcpy(wifi_cur_ssid, c->ssid, sizeof(wifi_cur_ssid));
    taskEXIT_CRITICAL(&state_mux);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     c->ssid, sizeof(cfg.sta.ssid)     - 1);
    strncpy((char *)cfg.sta.password, c->pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode =
        (strlen(c->pass) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    if (get_app_state() != APP_BRIDGE_NO_WIFI) {
        set_app_state(APP_WIFI_CONNECTING);
    }
    esp_wifi_connect();
}

static void handle_scan_done(void) {
    if (!scan_in_progress) return;   /* unsolicited (e.g. fast-scan internals) */
    scan_in_progress = false;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > SCAN_MAX_RECORDS) n = SCAN_MAX_RECORDS;

    wifi_ap_record_t *recs = NULL;
    if (n > 0) recs = calloc(n, sizeof(wifi_ap_record_t));
    if (recs != NULL) {
        uint16_t got = n;
        if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
            n = got;
        } else {
            n = 0;
        }
    } else {
        esp_wifi_clear_ap_list();
        n = 0;
    }

    /* candidates = scan hits ∩ saved list, dedup per SSID keeping best
     * RSSI, sorted best-first */
    wifi_list_t saved;
    taskENTER_CRITICAL(&state_mux);
    saved = wifi_list;
    taskEXIT_CRITICAL(&state_mux);

    cand_count = 0;
    for (uint16_t i = 0; i < n; i++) {
        const char *seen = (const char *)recs[i].ssid;
        for (int s = 0; s < saved.count; s++) {
            if (strcmp(seen, saved.e[s].ssid) != 0) continue;
            /* already a candidate? keep the better RSSI */
            int existing = -1;
            for (int c = 0; c < cand_count; c++) {
                if (strcmp(candidates[c].ssid, seen) == 0) { existing = c; break; }
            }
            if (existing >= 0) {
                if (recs[i].rssi > cand_rssi[existing]) {
                    cand_rssi[existing] = recs[i].rssi;
                }
            } else if (cand_count < WIFI_LIST_MAX) {
                candidates[cand_count] = saved.e[s];
                cand_rssi[cand_count]  = recs[i].rssi;
                cand_count++;
            }
            break;
        }
    }
    free(recs);

    /* insertion sort by RSSI descending (cand_count <= 8) */
    for (int i = 1; i < cand_count; i++) {
        wifi_cred_t c = candidates[i];
        int8_t r = cand_rssi[i];
        int j = i - 1;
        while (j >= 0 && cand_rssi[j] < r) {
            candidates[j + 1] = candidates[j];
            cand_rssi[j + 1]  = cand_rssi[j];
            j--;
        }
        candidates[j + 1] = c;
        cand_rssi[j + 1]  = r;
    }

    ESP_LOGI(TAG, "scan done: %u APs, %d saved-network candidates",
             n, cand_count);

    if (cand_count == 0) {
        wifi_cycle_failed();
        return;
    }
    cand_index = 0;
    cand_attempts = 0;
    wifi_try_candidate();
}

// ============================================================
// Retry / recovery tasks (satura's guarded-task pattern)
// ============================================================

static void wifi_schedule_retry(uint32_t delay_ms) {
    bool already;
    taskENTER_CRITICAL(&state_mux);
    already = wifi_retry_running;
    if (!already) wifi_retry_running = true;
    taskEXIT_CRITICAL(&state_mux);
    if (already) return;

    if (xTaskCreate(wifi_retry_task, "wr", 3072,
                    (void *)(uintptr_t)delay_ms, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create retry task");
        taskENTER_CRITICAL(&state_mux);
        wifi_retry_running = false;
        taskEXIT_CRITICAL(&state_mux);
    }
}

static void wifi_retry_task(void *arg) {
    uint32_t delay = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay));

    taskENTER_CRITICAL(&state_mux);
    wifi_retry_running = false;
    taskEXIT_CRITICAL(&state_mux);

    /* List may have been cleared by /reset while we slept */
    if (wifi_list_count() > 0 && !get_wifi_connected()) {
        wifi_connect_begin();
    }
    vTaskDelete(NULL);
}

static void wifi_recovery_task(void *arg) {
    (void)arg;
    wifi_soft_reset();
    taskENTER_CRITICAL(&state_mux);
    wifi_recovery_running = false;
    taskEXIT_CRITICAL(&state_mux);
    vTaskDelete(NULL);
}

/* Called each watchdog heartbeat: trigger recovery if WiFi is stuck. */
void wifi_watchdog_check(void) {
    static uint32_t wifi_stuck_count = 0;

    if (get_app_state() == APP_BRIDGE_NO_WIFI) {
        if (++wifi_stuck_count >= 10) {
            ESP_LOGE(TAG, "[WDT] WiFi stuck! Triggering recovery...");
            bool already;
            taskENTER_CRITICAL(&state_mux);
            already = wifi_recovery_running;
            if (!already) wifi_recovery_running = true;
            taskEXIT_CRITICAL(&state_mux);
            if (!already) {
                gw_safe_task_create(wifi_recovery_task, "wifi_rec",
                                    3072, NULL, 5, NULL);
            }
            wifi_stuck_count = 0;
        }
    } else {
        wifi_stuck_count = 0;
    }
}

// ============================================================
// Event handler
// ============================================================

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        handle_scan_done();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {

        taskENTER_CRITICAL(&state_mux);
        bool ignore = wifi_ignore_disconnect;
        if (ignore) wifi_ignore_disconnect = false;
        wifi_connected = false;
        taskEXIT_CRITICAL(&state_mux);

        update_nat();
        taskENTER_CRITICAL(&state_mux);
        strlcpy(wifi_ip, "--", sizeof(wifi_ip));
        taskEXIT_CRITICAL(&state_mux);

        if (ignore) return;

        app_state_t st = get_app_state();

        if (st == APP_WIFI_CONNECTING || st == APP_BRIDGE_NO_WIFI) {
            /* mid candidate cycle? */
            if (cand_count > 0) {
                cand_attempts++;
                if (cand_attempts < CAND_MAX_ATTEMPTS) {
                    esp_wifi_connect();     /* same candidate, retry */
                    return;
                }
                cand_attempts = 0;
                cand_index++;
                if (cand_index < cand_count) {
                    wifi_try_candidate();
                    return;
                }
                cand_count = 0;             /* cycle exhausted */
            }
            wifi_cycle_failed();

        } else if (st == APP_BRIDGE) {
            if (wifi_list_count() == 0) {
                set_app_state(APP_NO_WIFI);
                return;
            }
            set_app_state(APP_BRIDGE_NO_WIFI);
            taskENTER_CRITICAL(&state_mux);
            wifi_retries = 0;
            wifi_retry_delay_ms = WIFI_RETRY_BASE_MS;
            taskEXIT_CRITICAL(&state_mux);
            cand_count = 0;
            wifi_schedule_retry(WIFI_RETRY_BASE_MS);
        }
        /* other states (deliberate disconnects etc.): nothing to do */

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;

        taskENTER_CRITICAL(&state_mux);
        snprintf(wifi_ip, sizeof(wifi_ip), IPSTR, IP2STR(&e->ip_info.ip));
        taskEXIT_CRITICAL(&state_mux);

        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            taskENTER_CRITICAL(&state_mux);
            wifi_rssi = ap.rssi;
            taskEXIT_CRITICAL(&state_mux);
        }

        taskENTER_CRITICAL(&state_mux);
        wifi_connected      = true;
        wifi_retries        = 0;
        wifi_retry_delay_ms = WIFI_RETRY_BASE_MS;
        taskEXIT_CRITICAL(&state_mux);
        cand_count = 0;

        ESP_LOGI(TAG, "got IP, connected to '%s'", wifi_cur_ssid);
        update_nat();

        if (ppp_link_up()) {
            set_app_state(APP_BRIDGE);
        } else if (get_app_state() != APP_WAIT_BT) {
            set_app_state(APP_WAIT_BT);
        }
    }
}

// ============================================================
// Init & soft reset
// ============================================================

static void wifi_register_handlers(void) {
    if (wifi_evt_inst == NULL)
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            wifi_event_handler, NULL, &wifi_evt_inst));
    if (ip_evt_inst == NULL)
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            wifi_event_handler, NULL, &ip_evt_inst));
}

static void wifi_unregister_handlers(void) {
    if (wifi_evt_inst) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt_inst);
        wifi_evt_inst = NULL;
    }
    if (ip_evt_inst) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, ip_evt_inst);
        ip_evt_inst = NULL;
    }
}

void wifi_multi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_register_handlers();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_list.ver = 1;
    if (nvs_list_load() && wifi_list_count() > 0) {
        ESP_LOGI(TAG, "%d saved network(s) — WiFi connects after PPP is up",
                 wifi_list_count());
        /* stay in APP_WAIT_BT: the connect kicks in after the phone dials */
    } else {
        set_app_state(APP_NO_WIFI);
    }
}

static void wifi_soft_reset(void) {
    ESP_LOGW(TAG, "soft-reset starting...");

    wifi_unregister_handlers();

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_deinit();
    vTaskDelay(pdMS_TO_TICKS(300));

    scan_in_progress = false;
    cand_count = 0;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    wifi_register_handlers();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "soft-reset done, restarting connect cycle");
    if (wifi_list_count() > 0) {
        wifi_connect_begin();
    }
}
