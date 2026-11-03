/*
 * web_ui.c — setup/status pages at http://192.168.7.1 over the PPP link.
 *
 * Must render on Opera for S60v1: plain HTML, simple forms, no JS/CSS
 * dependencies. Skeleton (captive redirect, no-cache, html_escape, 404→302)
 * lifted from satura-bridge.
 */

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

#include "gateway.h"

static const char *TAG = "web_ui";

static httpd_handle_t http_server = NULL;

#define PAGE_FOOTER \
    "<hr><small>Vetera Bridge - internet for pre-PAN Bluetooth phones</small>"

// ============================================================
// Helpers (satura)
// ============================================================

static void html_escape(const char *src, char *dst, size_t n) {
    size_t w = 0;
    for (const char *p = src; *p && w + 7 < n; p++) {
        const char *rep = NULL;
        if      (*p == '&') rep = "&amp;";
        else if (*p == '<') rep = "&lt;";
        else if (*p == '>') rep = "&gt;";
        else if (*p == '"') rep = "&quot;";
        if (rep) { size_t l = strlen(rep); memcpy(dst + w, rep, l); w += l; }
        else dst[w++] = *p;
    }
    dst[w] = 0;
}

/* Append to a page buffer at offset w, returning the new offset.
 *
 * snprintf returns the length it *wanted* to write, so the obvious
 * `w += snprintf(page + w, n - w, ...)` chain starts writing past the end and
 * passing an underflowed size_t the moment one call truncates. Clamping here
 * makes a too-small buffer clip the page instead of corrupting the heap. */
static int page_add(char *dst, size_t n, int w, const char *fmt, ...) {
    if (w < 0 || (size_t)w >= n) return (int)n - 1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst + w, n - w, fmt, ap);
    va_end(ap);
    if (r < 0) return w;
    w += r;
    return ((size_t)w >= n) ? (int)n - 1 : w;
}

/* Bytes html_escape() will produce for src, excluding the NUL. Lets a page
 * buffer be sized exactly: a bookmark URL is mostly query string, and every
 * '&' expands 5x — guessing here truncates hrefs into dead links. */
static size_t html_escape_len(const char *src) {
    size_t n = 0;
    for (const char *p = src; *p; p++) {
        switch (*p) {
            case '&':            n += 5; break;
            case '<': case '>':  n += 4; break;
            case '"':            n += 6; break;
            default:             n += 1; break;
        }
    }
    return n;
}

/* application/x-www-form-urlencoded: '+' means space, %XX escapes */
static void form_decode(const char *src, char *dst, size_t n) {
    size_t w = 0;
    for (const char *p = src; *p && w + 1 < n; p++) {
        if (*p == '+') { dst[w++] = ' '; continue; }
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            dst[w++] = (char)strtol(hex, NULL, 16);
            p += 2;
            continue;
        }
        dst[w++] = *p;
    }
    dst[w] = 0;
}

/* Read a whole POST body. httpd_req_recv() is free to return a short read,
 * and a bookmark form (urlencoded title + a query-string URL) runs well past
 * one read — a single recv() silently drops the tail and stores a mangled
 * entry. Returns bytes read, or -1. */
static int recv_body(httpd_req_t *req, char *buf, size_t size) {
    size_t want = req->content_len;
    if (want > size - 1) want = size - 1;

    size_t total = 0;
    while (total < want) {
        int r = httpd_req_recv(req, buf + total, want - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    buf[total] = '\0';
    return (int)total;
}

static void set_no_cache(httpd_req_t *req, const char *type) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control",
                       "no-cache, no-store, must-revalidate");
}

static esp_err_t redirect_to(httpd_req_t *req, const char *loc) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    /* Old Opera (S60) will happily cache a bare 302 and keep replaying it
     * for that URL even after the bridge comes back online — belt and
     * braces since Cache-Control alone isn't always honored. */
    httpd_resp_set_hdr(req, "Cache-Control",
                       "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    return httpd_resp_send(req, NULL, 0);
}

/* Any request whose Host header names a domain rather than the gateway
 * itself gets bounced to the gateway IP — this also catches phones that
 * cached the gateway's own IP for a real domain while we were offline
 * and are still connecting to us out of habit after WiFi comes back. */
static bool captive_check(httpd_req_t *req) {
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK
        && strstr(host, GW_IP_STR) == NULL) {
        char host_no_port[64];
        strncpy(host_no_port, host, sizeof(host_no_port) - 1);
        host_no_port[sizeof(host_no_port)-1] = '\0';
        char *colon = strchr(host_no_port, ':');
        if (colon) *colon = '\0';
        struct in_addr tmp;
        if (inet_pton(AF_INET, host_no_port, &tmp) == 0) {
            redirect_to(req, "http://" GW_IP_STR "/");
            return false;
        }
    }
    return true;
}

// ============================================================
// Bookmarks — shared helpers
// ============================================================

/* Total escaped bytes of every title+url in the list. Page buffers are sized
 * from this rather than a per-entry guess, so a URL full of '&' can't clip
 * the page mid-tag. Costs one extra NVS read per entry; a page a human asked
 * for can afford it, and it keeps peak RAM at one entry. */
static size_t bm_escaped_total(void) {
    size_t n = 0;
    bookmark_t b;
    int count = bookmarks_count();
    for (int i = 0; i < count; i++) {
        if (!bookmarks_get(i, &b)) continue;
        n += html_escape_len(b.title) + html_escape_len(b.url);
    }
    return n;
}

/* Escape one bookmark. label_out points into one of the two buffers: the
 * title when there is one, the URL otherwise. */
static void bm_escape(const bookmark_t *b, char *url_esc, size_t url_n,
                      char *title_esc, size_t title_n, const char **label_out) {
    html_escape(b->url, url_esc, url_n);
    if (b->title[0]) {
        html_escape(b->title, title_esc, title_n);
        *label_out = title_esc;
    } else {
        *label_out = url_esc;
    }
}

/* The bookmark links themselves, rendered above the status block: the whole
 * point of the feature is that the phone opens 192.168.7.1 and taps, with no
 * typing and no extra page load. Returns bytes written. */
static int bm_render_links(char *dst, size_t n) {
    int count = bookmarks_count();
    if (count == 0) return 0;

    int w = page_add(dst, n, 0, "<div style='text-align:left;padding:8px 0;'>");
    for (int i = 0; i < count; i++) {
        bookmark_t b;
        if (!bookmarks_get(i, &b)) continue;
        char url_esc[BM_URL_MAX * 6], title_esc[BM_TITLE_MAX * 6];
        const char *label;
        bm_escape(&b, url_esc, sizeof(url_esc),
                  title_esc, sizeof(title_esc), &label);
        /* double quotes around the href: html_escape covers " but not ' */
        w = page_add(dst, n, w,
            "<a href=\"%s\" style='font-size:120%%;'>%s</a><br>",
            url_esc, label);
    }
    return page_add(dst, n, w, "</div><hr>");
}

// ============================================================
// Pages
// ============================================================

static esp_err_t handler_root(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    app_state_t st = get_app_state();
    if (wifi_list_count() == 0 &&
        (st == APP_WAIT_BT || st == APP_NO_WIFI)) {
        return redirect_to(req, "/wifi");
    }

    char ssid[33], ip[16], esc[80];
    wifi_get_current(ssid, sizeof(ssid), ip, sizeof(ip));
    html_escape(ssid, esc, sizeof(esc));
    char name[BT_NAME_MAX], name_esc[BT_NAME_MAX * 6];
    bt_name_get(name, sizeof(name));
    html_escape(name, name_esc, sizeof(name_esc));
    uint32_t up = uptime_seconds();

    /* Auto-refresh only while something is actually changing. The old
     * unconditional 30 s reload would yank the page away mid-scroll from
     * someone picking a bookmark. */
    bool busy = (st == APP_WIFI_SCANNING || st == APP_WIFI_CONNECTING ||
                 st == APP_BRIDGE_NO_WIFI);

    char rssi_buf[8];
    int bmc = bookmarks_count();
    size_t sz = 3072 + bm_escaped_total() + (size_t)bmc * 64;
    char *page = malloc(sz);
    if (!page) return ESP_ERR_NO_MEM;

    int w = page_add(page, sz, 0,
        "<html><head><title>%s</title>"
        "<meta charset='utf-8'>"
        "%s"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>%s</h2><hr>",
        name_esc,
        busy ? "<meta http-equiv='refresh' content='5'>" : "",
        name_esc);

    w += bm_render_links(page + w, sz - w);

    page_add(page, sz, w,
        "<div style='text-align:left;background:#ecf0f1;padding:15px;'>"
        "<b>Status:</b> %s<br>"
        "<b>Phone link (PPP):</b> %s<br>"
        "<b>WiFi:</b> %s %s<br>"
        "<b>WiFi IP:</b> %s<br>"
        "<b>WiFi RSSI:</b> %d dBm<br>"
        "<b>BT RSSI:</b> %s<br>"
        "<b>Saved networks:</b> %d/%d<br>"
        "<b>Bookmarks:</b> %d/%d<br>"
        "<b>Uptime:</b> %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 "<br>"
        "<b>Free heap:</b> %d KB"
        "</div><hr>"
        "<a href='/'>Reload</a><br>"
        "<a href='/bm'>Bookmarks</a><br>"
        "<a href='/wifi'>WiFi networks</a><br>"
        "<a href='/name'>Device name</a><br>"
        "<a href='/security'>Bluetooth security</a><br>"
        "<a href='/debug'>Debug logging</a><br>"
        "<a href='/reboot' style='color:#e74c3c;'>Reboot</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        state_to_str(st),
        ppp_link_up() ? "up" : (get_bt_connected() ? "negotiating" : "down"),
        get_wifi_connected() ? "connected to" : "not connected",
        get_wifi_connected() ? esc : "",
        ip,
        (int)wifi_get_rssi(),
        bt_rssi_str(rssi_buf, sizeof(rssi_buf)),
        wifi_list_count(), WIFI_LIST_MAX,
        bmc, BM_MAX,
        up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60,
        (int)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

// ============================================================
// Bookmark pages
// ============================================================

/* Manage page — mainly used from a PC on the WiFi side, where there is a real
 * keyboard. Reachable from the phone too; the pages are identical either way
 * (captive_check lets an IP-literal Host through untouched). */
static esp_err_t handler_bm_get(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    /* ?err=1 comes back from a rejected save (bad or over-long address) */
    char query[32] = {0}, ev[8] = {0};
    bool err = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
            && httpd_query_key_value(query, "err", ev, sizeof(ev)) == ESP_OK;

    int count = bookmarks_count();
    /* url appears twice per row (href + visible text), title once */
    size_t sz = 3072 + bm_escaped_total() * 2 + (size_t)count * 160;
    char *page = malloc(sz);
    if (!page) return ESP_ERR_NO_MEM;

    int w = page_add(page, sz, 0,
        "<html><head><title>Bookmarks</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>Bookmarks</h2><hr>");

    if (err) {
        w = page_add(page, sz, w,
            "<p style='color:#e74c3c;'>Not saved.<br>"
            "The address must be http:// or https:// and at most %d "
            "characters.</p>", BM_URL_MAX - 1);
    }

    if (count == 0) {
        w = page_add(page, sz, w,
            "<p>No bookmarks yet.<br>"
            "Add them from a PC on the same WiFi at this bridge's WiFi IP "
            "(shown on the status page) - then they are one tap away on the "
            "phone.</p>");
    } else {
        w = page_add(page, sz, w,
            "<div style='text-align:left;background:#ecf0f1;padding:15px;'>");
        for (int i = 0; i < count; i++) {
            bookmark_t b;
            if (!bookmarks_get(i, &b)) continue;
            char url_esc[BM_URL_MAX * 6], title_esc[BM_TITLE_MAX * 6];
            const char *label;
            bm_escape(&b, url_esc, sizeof(url_esc),
                      title_esc, sizeof(title_esc), &label);
            w = page_add(page, sz, w,
                "%d. <a href=\"%s\"><b>%s</b></a><br>"
                "<small>%s</small><br>"
                "[<a href='/bm/edit?i=%d'>edit</a>] "
                "[<a href='/bm/del?i=%d'>delete</a>]<br><br>",
                i + 1, url_esc, label, url_esc, i, i);
        }
        w = page_add(page, sz, w, "</div>");
    }

    page_add(page, sz, w,
        "<p>(%d/%d used)</p>"
        "%s"
        "<hr>"
        "<a href='/'>Status</a><br>"
        "<a href='/wifi'>WiFi networks</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        count, BM_MAX,
        count < BM_MAX ? "<a href='/bm/edit' style='font-size:120%'>"
                         "Add a bookmark</a>"
                       : "<p>List is full - delete one first.</p>");

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

/* Add form (no ?i) or edit form (?i=N, prefilled). */
static esp_err_t handler_bm_edit(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    int index = -1;
    char query[32] = {0}, val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "i", val, sizeof(val)) == ESP_OK) {
        index = atoi(val);
    }

    bookmark_t b = {0};
    bool editing = (index >= 0) && bookmarks_get(index, &b);
    if (!editing) index = -1;

    if (!editing && bookmarks_count() >= BM_MAX) {
        return redirect_to(req, "/bm");
    }

    char url_esc[BM_URL_MAX * 6], title_esc[BM_TITLE_MAX * 6];
    html_escape(b.url, url_esc, sizeof(url_esc));
    html_escape(b.title, title_esc, sizeof(title_esc));

    size_t sz = 2048 + sizeof(url_esc) + sizeof(title_esc);
    char *page = malloc(sz);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, sz,
        "<html><head><title>%s Bookmark</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>%s Bookmark</h2><hr>"
        "<form action='/bm/save' method='post'>"
        "<input type='hidden' name='i' value='%d'>"
        "<p>Name:<br>"
        /* double quotes around value: html_escape covers " but not ' */
        "<input type='text' name='title' size='20' maxlength='%d' value=\"%s\"></p>"
        "<p>Address:<br>"
        "<input type='text' name='url' size='30' maxlength='%d' value=\"%s\"></p>"
        "<p><input type='submit' value='Save' style='font-size:110%%;'></p>"
        "</form>"
        "<p style='text-align:left;'><small>http:// is added automatically if "
        "you leave it off. HTTPS sites will not load on an S60v1 phone - its "
        "TLS is too old - so plain http:// addresses are the useful ones.</small></p>"
        "<hr>"
        "<a href='/bm'>Bookmarks</a><br>"
        "<a href='/'>Status</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        editing ? "Edit" : "Add",
        editing ? "Edit" : "Add",
        index,
        BM_TITLE_MAX - 1, title_esc,
        BM_URL_MAX - 1, url_esc);

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

static esp_err_t handler_bm_save(httpd_req_t *req) {
    char buf[1024] = {0};
    if (recv_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    char ri[8] = {0}, rt[128] = {0}, ru[512] = {0};
    if (httpd_query_key_value(buf, "url", ru, sizeof(ru)) != ESP_OK) {
        return redirect_to(req, "/bm");
    }
    httpd_query_key_value(buf, "i", ri, sizeof(ri));
    httpd_query_key_value(buf, "title", rt, sizeof(rt));

    char title[128] = {0}, url[512] = {0};
    form_decode(rt, title, sizeof(title));
    form_decode(ru, url, sizeof(url));

    if (!bookmarks_set(ri[0] ? atoi(ri) : -1, title, url)) {
        return redirect_to(req, "/bm?err=1");
    }
    return redirect_to(req, "/bm");
}

static esp_err_t handler_bm_del(httpd_req_t *req) {
    char query[32] = {0}, val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "i", val, sizeof(val)) == ESP_OK) {
        bookmarks_remove(atoi(val));
    }
    return redirect_to(req, "/bm");
}

static esp_err_t handler_wifi_get(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    int count = wifi_list_count();
    app_state_t st = get_app_state();

    char *page = malloc(4096);
    if (!page) return ESP_ERR_NO_MEM;
    int w = page_add(page, 4096, 0,
        "<html><head><title>WiFi Networks</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>WiFi Networks</h2><hr>");

    if (st == APP_WIFI_FAILED) {
        w = page_add(page, 4096, w,
            "<p style='color:#e74c3c;'>Could not connect to any saved "
            "network.<br>Check the list below.</p>");
    }

    if (count == 0) {
        w = page_add(page, 4096, w,
            "<p>No networks saved yet.<br>"
            "The gateway connects to the strongest saved network "
            "it can find.</p>");
    } else {
        w = page_add(page, 4096, w,
            "<div style='text-align:left;background:#ecf0f1;padding:15px;'>");
        for (int i = 0; i < count; i++) {
            char ssid[33], esc[80];
            if (!wifi_list_get_ssid(i, ssid, sizeof(ssid))) continue;
            html_escape(ssid, esc, sizeof(esc));
            w = page_add(page, 4096, w,
                "%d. <b>%s</b> [<a href='/wifi/del?i=%d'>delete</a>]<br>",
                i + 1, esc, i);
        }
        w = page_add(page, 4096, w, "</div>");
    }

    page_add(page, 4096, w,
        "<p>(%d/%d used)</p>"
        "<form action='/wifi/add' method='post'>"
        "<p>SSID:<br>"
        "<input type='text' name='ssid' size='20' maxlength='32'></p>"
        "<p>Password: (empty for open networks)<br>"
        "<input type='password' name='pass' size='20' maxlength='63'></p>"
        "<p><input type='submit' value='Add network' style='font-size:110%%;'></p>"
        "</form><hr>"
        "<a href='/'>Status</a><br>"
        "<a href='/reset'>Forget all networks</a><br>"
        "<a href='/reboot' style='color:#e74c3c;'>Reboot</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        count, WIFI_LIST_MAX);

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

static esp_err_t handler_wifi_add(httpd_req_t *req) {
    char buf[256] = {0};
    if (recv_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    char ns[64] = {0}, np[80] = {0};
    if (httpd_query_key_value(buf, "ssid", ns, sizeof(ns)) == ESP_OK) {
        httpd_query_key_value(buf, "pass", np, sizeof(np));

        char ssid[64] = {0}, pass[80] = {0};
        form_decode(ns, ssid, sizeof(ssid));
        form_decode(np, pass, sizeof(pass));

        if (wifi_list_add(ssid, pass)) {
            ESP_LOGI(TAG, "added network '%s'", ssid);
            if (!get_wifi_connected() && wifi_list_count() > 0 &&
                ppp_link_up()) {
                /* Deferred: an immediate scan steals the shared radio
                 * before the redirect below reaches the phone, and the
                 * browser hangs on the save. */
                gw_wifi_kick_deferred();
            }
        }
    }
    return redirect_to(req, "/wifi");
}

static esp_err_t handler_wifi_del(httpd_req_t *req) {
    char query[32] = {0}, val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "i", val, sizeof(val)) == ESP_OK) {
        wifi_list_remove(atoi(val));
    }
    return redirect_to(req, "/wifi");
}

static esp_err_t handler_name_get(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    char cur[BT_NAME_MAX], custom[BT_NAME_MAX], derived[BT_NAME_MAX];
    bt_name_get(cur, sizeof(cur));
    bt_name_get_custom(custom, sizeof(custom));
    bt_name_get_derived(derived, sizeof(derived));

    char cur_esc[BT_NAME_MAX * 6], custom_esc[BT_NAME_MAX * 6];
    char derived_esc[BT_NAME_MAX * 6];
    html_escape(cur, cur_esc, sizeof(cur_esc));
    html_escape(custom, custom_esc, sizeof(custom_esc));
    html_escape(derived, derived_esc, sizeof(derived_esc));

    char *page = malloc(2560);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, 2560,
        "<html><head><title>Device Name</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>Device Name</h2><hr>"
        "<div style='text-align:left;background:#ecf0f1;padding:15px;'>"
        "<b>Shown to phones:</b> %s<br>"
        "<b>Default for this unit:</b> %s"
        "</div>"
        "<form action='/name' method='post'>"
        "<p>Custom name: (empty = use the default)<br>"
        /* double quotes around value: html_escape covers " but not ' */
        "<input type='text' name='name' size='20' maxlength='%d' value=\"%s\"></p>"
        "<p><input type='submit' value='Save name' style='font-size:110%%;'></p>"
        "</form>"
        "<p style='text-align:left;'><small>The default number comes from this "
        "unit's Bluetooth address, so every bridge differs. A phone caches the "
        "name in its paired-devices list - it may keep showing the old one until "
        "it searches for devices again.</small></p><hr>"
        "<a href='/'>Status</a><br>"
        "<a href='/wifi'>WiFi networks</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        cur_esc,
        derived[0] ? derived_esc : "(waiting for Bluetooth)",
        BT_NAME_CUSTOM_MAX, custom_esc);

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

static esp_err_t handler_name_post(httpd_req_t *req) {
    char buf[192] = {0};
    if (recv_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    char raw[128] = {0};
    if (httpd_query_key_value(buf, "name", raw, sizeof(raw)) == ESP_OK) {
        char name[128] = {0};
        form_decode(raw, name, sizeof(name));
        bt_name_set(name);
    }
    return redirect_to(req, "/name");
}

// ============================================================
// Debug logging page
// ============================================================

static esp_err_t handler_debug_get(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    uint8_t f = debug_log_get();
    char *page = malloc(2560);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, 2560,
        "<html><head><title>Debug Logging</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>Debug Logging</h2><hr>"
        "<form action='/debug' method='post' style='text-align:left;'>"
        "<p><input type='checkbox' name='hci' value='1'%s> "
        "<b>HCI packet trace</b><br>"
        "<small>Every Bluetooth HCI packet as hex: SDP queries, RFCOMM "
        "setup, pairing.</small></p>"
        "<p><input type='checkbox' name='stk' value='1'%s> "
        "<b>BTstack internal log</b><br>"
        "<small>The Bluetooth stack's own L2CAP/RFCOMM/SDP state "
        "messages.</small></p>"
        "<p><input type='checkbox' name='vrb' value='1'%s> "
        "<b>Verbose bridge events</b><br>"
        "<small>Unhandled HCI event codes, link security when a phone "
        "opens the serial channel, every DNS query and its answer.</small></p>"
        "<p style='text-align:center;'><input type='submit' value='Save' "
        "style='font-size:110%%;'></p>"
        "</form>"
        "<p style='text-align:left;'><small>Output goes to the USB serial "
        "console (115200 baud) only. Changes apply at once and are kept "
        "across reboots, so a trace can cover pairing from power-on. The "
        "packet trace slows the phone's connection a lot: turn it off for "
        "normal use.</small></p><hr>"
        "<a href='/'>Status</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        (f & DBG_HCI_PACKETS) ? " checked" : "",
        (f & DBG_BTSTACK_LOG) ? " checked" : "",
        (f & DBG_VERBOSE)     ? " checked" : "");

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

static esp_err_t handler_debug_post(httpd_req_t *req) {
    char buf[128] = {0};
    if (recv_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    /* Unticked checkboxes are simply absent from the form body. */
    char v[4];
    uint8_t f = 0;
    if (httpd_query_key_value(buf, "hci", v, sizeof(v)) == ESP_OK) f |= DBG_HCI_PACKETS;
    if (httpd_query_key_value(buf, "stk", v, sizeof(v)) == ESP_OK) f |= DBG_BTSTACK_LOG;
    if (httpd_query_key_value(buf, "vrb", v, sizeof(v)) == ESP_OK) f |= DBG_VERBOSE;
    debug_log_set(f);
    return redirect_to(req, "/debug");
}

// ============================================================
// Bluetooth security page
// ============================================================

static esp_err_t handler_security_get(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    btsec_mode_t mode = ppp_link_secure_get();
    const char *now = mode == BTSEC_ON  ? "required" :
                      mode == BTSEC_OFF ? "off" : "auto (by phone name)";

    /* In auto, show what the last phone was taken for: a renamed phone
     * explains itself here. */
    char last[BT_NAME_MAX * 6 + 64] = "";
    char name[BT_NAME_MAX];
    bool secured;
    if (mode == BTSEC_AUTO && ppp_link_last_phone(name, sizeof(name), &secured)) {
        char esc[BT_NAME_MAX * 6];
        html_escape(name[0] ? name : "(no name)", esc, sizeof(esc));
        snprintf(last, sizeof(last), "<br><b>Last phone:</b> %s, %s", esc,
                 secured ? "secured" : "known model, not secured");
    }

    char *page = malloc(3072);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, 3072,
        "<html><head><title>Bluetooth Security</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>Bluetooth Security</h2><hr>"
        "<div style='text-align:left;background:#ecf0f1;padding:15px;'>"
        "<b>Now:</b> %s%s"
        "</div>"
        "<form action='/security' method='post' style='text-align:left;'>"
        "<p><input type='radio' name='sec' value='auto'%s> <b>Auto</b><br>"
        "<small>Off for S60 1st Edition phones and Nokia Communicators, "
        "recognised by their Bluetooth name (N-Gage, 3650, 3660, 7650, "
        "9210, 9290, 9300, 9500), on for every other "
        "phone. A phone renamed away from its model counts as \"other\".</small></p>"
        "<p><input type='radio' name='sec' value='on'%s> "
        "<b>Require authentication and encryption</b><br>"
        "<small>The bridge authenticates and encrypts the link before a "
        "phone can open the serial channel. S60 2nd Edition phones (e.g. "
        "Nokia 7610) with gnubox need this.</small></p>"
        "<p><input type='radio' name='sec' value='off'%s> <b>Off</b><br>"
        "<small>How S60 1st Edition phones (N-Gage, 3650, ...) connect.</small></p>"
        "<p style='text-align:center;'><input type='submit' value='Save' "
        "style='font-size:110%%;'></p>"
        "</form>"
        "<p style='text-align:left;'><small>Saving a change reboots the "
        "bridge, which drops a connected phone. With security on for a "
        "phone, one that has not paired with the bridge is asked for the "
        "PIN (0000) when it connects. Holding the board's BOOT button for "
        "3 s also switches it: off, on, auto in turn.</small></p><hr>"
        "<a href='/'>Status</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        now, last,
        mode == BTSEC_AUTO ? " checked" : "",
        mode == BTSEC_ON   ? " checked" : "",
        mode == BTSEC_OFF  ? " checked" : "");

    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

static void reboot_task(void *arg);

static esp_err_t handler_security_post(httpd_req_t *req) {
    char buf[64] = {0};
    if (recv_body(req, buf, sizeof(buf)) < 0) return ESP_FAIL;

    char v[8];
    if (httpd_query_key_value(buf, "sec", v, sizeof(v)) != ESP_OK) {
        return redirect_to(req, "/security");
    }
    btsec_mode_t mode;
    if      (strcmp(v, "on") == 0)   mode = BTSEC_ON;
    else if (strcmp(v, "off") == 0)  mode = BTSEC_OFF;
    else if (strcmp(v, "auto") == 0) mode = BTSEC_AUTO;
    else return redirect_to(req, "/security");
    if (mode == ppp_link_secure_get()) return redirect_to(req, "/security");

    /* BTstack only takes the level at boot, so apply by rebooting. */
    ppp_link_secure_save(mode);
    httpd_resp_sendstr(req,
        "<html><body><p>Saved. Rebooting...</p></body></html>");
    gw_safe_task_create(reboot_task, "reboot", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t handler_reset(httpd_req_t *req) {
    wifi_list_clear();
    return redirect_to(req, "/wifi");
}

static void reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handler_reboot(httpd_req_t *req) {
    httpd_resp_sendstr(req,
        "<html><body><p>Rebooting...</p></body></html>");
    gw_safe_task_create(reboot_task, "reboot", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t handler_favicon(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handler_404(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    return redirect_to(req, "http://" GW_IP_STR "/");
}

void web_ui_start(void) {
    static const httpd_uri_t uris[] = {
        { "/",            HTTP_GET,  handler_root,     NULL },
        { "/wifi",        HTTP_GET,  handler_wifi_get, NULL },
        { "/wifi/add",    HTTP_POST, handler_wifi_add, NULL },
        { "/wifi/del",    HTTP_GET,  handler_wifi_del, NULL },
        { "/bm",          HTTP_GET,  handler_bm_get,   NULL },
        { "/bm/edit",     HTTP_GET,  handler_bm_edit,  NULL },
        { "/bm/save",     HTTP_POST, handler_bm_save,  NULL },
        { "/bm/del",      HTTP_GET,  handler_bm_del,   NULL },
        { "/name",        HTTP_GET,  handler_name_get, NULL },
        { "/name",        HTTP_POST, handler_name_post, NULL },
        { "/debug",       HTTP_GET,  handler_debug_get, NULL },
        { "/debug",       HTTP_POST, handler_debug_post, NULL },
        { "/security",    HTTP_GET,  handler_security_get, NULL },
        { "/security",    HTTP_POST, handler_security_post, NULL },
        { "/reset",       HTTP_GET,  handler_reset,    NULL },
        { "/reboot",      HTTP_GET,  handler_reboot,   NULL },
        { "/favicon.ico", HTTP_GET,  handler_favicon,  NULL },
    };
    const size_t n_uris = sizeof(uris) / sizeof(uris[0]);

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    /* 10 KB, not the old 8: rendering a full bookmark list nests
     * handler_root -> bm_render_links (url_esc[960] + title_esc[192])
     * -> bookmarks_get -> NVS, and a stack overflow there would only ever
     * show up the moment someone opens a page full of bookmarks. */
    cfg.stack_size       = 10240;
    cfg.max_open_sockets = 2;
    /* The default is 8 slots. Registering past that fails with only a
     * warning, and the missing pages then fall through to the 404 redirect
     * to "/", which is how /name, /reset and /reboot quietly vanished. */
    cfg.max_uri_handlers = n_uris;
    if (httpd_start(&http_server, &cfg) == ESP_OK) {
        for (size_t i = 0; i < n_uris; i++)
            httpd_register_uri_handler(http_server, &uris[i]);
        httpd_register_err_handler(http_server,
                                   HTTPD_404_NOT_FOUND, handler_404);
        ESP_LOGI(TAG, "web UI at http://" GW_IP_STR "/");
    } else {
        ESP_LOGE(TAG, "httpd_start failed");
    }
}
