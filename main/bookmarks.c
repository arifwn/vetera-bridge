/*
 * bookmarks.c — a shared bookmark list, so a phone never has to type a URL.
 *
 * Typing an address on an S60v1 keypad is the worst part of using the bridge.
 * The list is edited from a PC on the WiFi side (http://<sta-ip>/bm) and shows
 * up as tappable links on the phone's http://192.168.7.1 home page.
 *
 * Storage: one NVS key per bookmark, "bm00".."bm<BM_MAX-1>", each a
 * variable-length string "<title>\n<url>", plus a "bmc" count. Deliberately
 * NOT one blob like wifi_multi.c's list: rewriting a ~5 KB blob needs ~2x that
 * free before NVS garbage-collects the old copy, and this partition also holds
 * BTstack's pairing link keys and the WiFi calibration data. Per-entry writes
 * touch ~100 bytes instead, so adding a bookmark can't starve pairing.
 *
 * The list is not mirrored in RAM. Unlike wifi_list, which the scan/connect
 * path consults constantly, bookmarks are only read while rendering a web
 * page — so entries are read from NVS one at a time and dropped. Only the
 * count is cached.
 *
 * No state_mux anywhere in here: esp_http_server runs every handler in one
 * task (max_open_sockets counts concurrent *connections*, not handlers), and
 * bookmarks_init() runs before the server starts. Don't "fix" this by copying
 * wifi_multi.c's critical sections.
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "gateway.h"

static const char *TAG = "bookmarks";

#define NVS_NAMESPACE   "gw"
#define NVS_KEY_COUNT   "bmc"

/* "<title>\n<url>" plus NUL */
#define BM_RAW_MAX      (BM_TITLE_MAX + BM_URL_MAX)

static int bm_count = 0;        /* cache of NVS_KEY_COUNT; NVS is authority */

/* Held open for the life of the app. Rendering a full list would otherwise
 * cost ~50 nvs_open/close pairs per page (sizing pass + render pass), each
 * walking the namespace table, on a page served over a slow Bluetooth link. */
static nvs_handle_t bm_nvs = 0;

/* Callers all validate index against bm_count first; the modulo is only so
 * the compiler can see the result is two digits (-Werror=format-truncation). */
static void entry_key(int index, char *out, size_t n) {
    snprintf(out, n, "bm%02u", (unsigned)index % 100u);
}

// ============================================================
// Sanitizing
// ============================================================

/* Copy src to dst dropping control characters and trimming outer spaces.
 * '\n' matters most: it is the record separator, and a raw one in a title
 * would silently split the entry on the next read. */
static void sanitize(const char *src, char *dst, size_t n) {
    size_t w = 0;
    while (*src == ' ' || *src == '\t') src++;
    for (const char *p = src; *p && w + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) continue;
        dst[w++] = (char)c;
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}

static bool starts_with_ci(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
    }
    return true;
}

/* True for something we're willing to prepend "http://" to: a bare host,
 * optionally with a numeric :port. Without this check "javascript:alert(1)"
 * has no "://" and would sail through the bare-host branch. */
static bool is_bare_host(const char *s) {
    const char *slash = strchr(s, '/');
    const char *colon = strchr(s, ':');
    if (colon == NULL) return true;
    if (slash != NULL && colon > slash) return true;   /* colon is in the path */

    const char *p = colon + 1;
    if (*p == '\0' || p == slash) return false;        /* "host:" with no port */
    for (; *p && p != slash; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

/* Normalize a submitted URL in place-ish: bare "frogfind.com" becomes
 * "http://frogfind.com". Without the scheme the browser treats the href as
 * relative, asks us for http://192.168.7.1/frogfind.com, and handler_404
 * bounces it back to "/" — the bookmark just looks broken. */
static bool normalize_url(const char *in, char *out, size_t n) {
    /* Sanitize into a roomier buffer than we can store, so an over-long
     * address is *rejected* rather than quietly truncated into a dead link. */
    char clean[BM_URL_MAX * 2];
    sanitize(in, clean, sizeof(clean));
    if (clean[0] == '\0') return false;
    if (strlen(clean) >= n) return false;

    if (strstr(clean, "://") == NULL) {
        if (!is_bare_host(clean)) return false;
        if (strlen(clean) + 7 >= n) return false;
        strlcpy(out, "http://", n);
        strlcat(out, clean, n);
        return true;
    }
    if (!starts_with_ci(clean, "http://") && !starts_with_ci(clean, "https://"))
        return false;
    strlcpy(out, clean, n);
    return true;
}

// ============================================================
// NVS
// ============================================================

static bool raw_read(int index, char *buf, size_t n) {
    char key[8];
    entry_key(index, key, sizeof(key));
    size_t len = n;
    return nvs_get_str(bm_nvs, key, buf, &len) == ESP_OK;
}

static esp_err_t raw_write(int index, const char *raw) {
    char key[8];
    entry_key(index, key, sizeof(key));
    return nvs_set_str(bm_nvs, key, raw);
}

static esp_err_t count_write(int count) {
    return nvs_set_u8(bm_nvs, NVS_KEY_COUNT, (uint8_t)count);
}

// ============================================================
// API
// ============================================================

void bookmarks_init(void) {
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &bm_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (%s) — bookmarks disabled",
                 esp_err_to_name(err));
        bm_nvs = 0;
        return;
    }

    uint8_t c = 0;
    if (nvs_get_u8(bm_nvs, NVS_KEY_COUNT, &c) == ESP_OK) {
        bm_count = (c > BM_MAX) ? BM_MAX : c;
    }

    /* Printed so the BM_MAX cap can be raised on evidence rather than a
     * guess — this partition is shared with WiFi calibration, nvs.net80211,
     * the WiFi list and BTstack's link keys. */
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        ESP_LOGI(TAG, "%d/%d bookmarks; NVS %d/%d entries used, %d free (~%d B)",
                 bm_count, BM_MAX, (int)st.used_entries, (int)st.total_entries,
                 (int)st.free_entries, (int)st.free_entries * 32);
    } else {
        ESP_LOGI(TAG, "%d/%d bookmarks", bm_count, BM_MAX);
    }
}

int bookmarks_count(void) {
    return bm_count;
}

bool bookmarks_get(int index, bookmark_t *out) {
    if (bm_nvs == 0 || index < 0 || index >= bm_count || out == NULL)
        return false;

    char raw[BM_RAW_MAX] = {0};
    if (!raw_read(index, raw, sizeof(raw))) return false;

    memset(out, 0, sizeof(*out));
    char *sep = strchr(raw, '\n');
    if (sep) {
        *sep = '\0';
        strlcpy(out->title, raw, sizeof(out->title));
        strlcpy(out->url, sep + 1, sizeof(out->url));
    } else {
        /* No separator: treat the whole record as the URL and let the caller
         * fall back to showing it as the label. */
        strlcpy(out->url, raw, sizeof(out->url));
    }
    return out->url[0] != '\0';
}

bool bookmarks_set(int index, const char *title, const char *url) {
    if (bm_nvs == 0 || url == NULL) return false;

    char clean_url[BM_URL_MAX];
    if (!normalize_url(url, clean_url, sizeof(clean_url))) return false;

    char clean_title[BM_TITLE_MAX];
    sanitize(title ? title : "", clean_title, sizeof(clean_title));

    bool append = (index < 0);
    if (append) {
        if (bm_count >= BM_MAX) {
            ESP_LOGW(TAG, "list full (%d)", BM_MAX);
            return false;
        }
        index = bm_count;
    } else if (index >= bm_count) {
        return false;
    }

    char raw[BM_RAW_MAX];
    snprintf(raw, sizeof(raw), "%s\n%s", clean_title, clean_url);

    esp_err_t err = raw_write(index, raw);
    /* Entry first, count second: a power cut between the two leaves an orphan
     * value nothing reads, rather than a counted entry that doesn't exist. */
    if (err == ESP_OK && append) err = count_write(index + 1);
    if (err == ESP_OK) err = nvs_commit(bm_nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    if (append) bm_count = index + 1;
    ESP_LOGI(TAG, "%s bookmark %d: %s", append ? "added" : "updated",
             index, clean_url);
    return true;
}

bool bookmarks_remove(int index) {
    if (bm_nvs == 0 || index < 0 || index >= bm_count) return false;

    int last = bm_count - 1;

    /* Count first here (the mirror of bookmarks_set): a power cut mid-shift
     * then hides one entry instead of exposing a slot that was already
     * erased. */
    esp_err_t err = count_write(last);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "delete failed: %s", esp_err_to_name(err));
        return false;
    }
    /* NVS applies writes immediately, so the shorter count is already on
     * flash — keep the RAM cache matching it whatever happens below. */
    bm_count = last;

    for (int j = index; j < last; j++) {
        char raw[BM_RAW_MAX] = {0};
        if (!raw_read(j + 1, raw, sizeof(raw))) continue;
        err = raw_write(j, raw);
        if (err != ESP_OK) {
            /* Out of NVS space mid-shift: the list is now one shorter but the
             * wrong entry went away. Loud, because this is the exact symptom
             * of a full partition and it must not pass silently. */
            ESP_LOGE(TAG, "delete: shift of slot %d failed (%s) — list may be "
                     "wrong, check bookmarks", j, esp_err_to_name(err));
            nvs_commit(bm_nvs);
            return false;
        }
    }

    char key[8];
    entry_key(last, key, sizeof(key));
    nvs_erase_key(bm_nvs, key);

    err = nvs_commit(bm_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "delete commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "removed bookmark %d (%d left)", index, bm_count);
    return true;
}
