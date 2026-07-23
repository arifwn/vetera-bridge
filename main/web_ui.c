/*
 * web_ui.c — setup/status pages at http://192.168.7.1 over the PPP link.
 *
 * Must render on Opera for S60v1: plain HTML, simple forms, no JS/CSS
 * dependencies. Skeleton (captive redirect, no-cache, html_escape, 404→302)
 * lifted from satura-bridge.
 */

#include <string.h>
#include <stdlib.h>
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
    "<hr><small>Vetera Bridge — internet for pre-PAN Bluetooth phones</small>"

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
    uint32_t up = uptime_seconds();
    int refresh = (st == APP_WIFI_SCANNING || st == APP_WIFI_CONNECTING ||
                   st == APP_BRIDGE_NO_WIFI) ? 5 : 30;

    char *page = malloc(3072);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, 3072,
        "<html><head><title>Vetera Bridge</title>"
        "<meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='%d'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>Vetera Bridge</h2><hr>"
        "<div style='text-align:left;background:#ecf0f1;padding:15px;'>"
        "<b>Status:</b> %s<br>"
        "<b>Phone link (PPP):</b> %s<br>"
        "<b>WiFi:</b> %s %s<br>"
        "<b>WiFi IP:</b> %s<br>"
        "<b>WiFi RSSI:</b> %d dBm<br>"
        "<b>BT RSSI:</b> %d dBm<br>"
        "<b>Saved networks:</b> %d/%d<br>"
        "<b>Uptime:</b> %" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 "<br>"
        "<b>Free heap:</b> %d KB"
        "</div><hr>"
        "<a href='/'>Reload</a><br>"
        "<a href='/wifi'>WiFi networks</a><br>"
        "<a href='/reboot' style='color:#e74c3c;'>Reboot</a>"
        "<br>" PAGE_FOOTER
        "</body></html>",
        refresh,
        state_to_str(st),
        ppp_link_up() ? "up" : (get_bt_connected() ? "negotiating" : "down"),
        get_wifi_connected() ? "connected to" : "not connected",
        get_wifi_connected() ? esc : "",
        ip,
        (int)wifi_get_rssi(),
        (int)get_bt_rssi(),
        wifi_list_count(), WIFI_LIST_MAX,
        up / 86400, (up % 86400) / 3600, (up % 3600) / 60, up % 60,
        (int)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    esp_err_t r = httpd_resp_sendstr(req, page);
    free(page);
    return r;
}

static esp_err_t handler_wifi_get(httpd_req_t *req) {
    if (!captive_check(req)) return ESP_OK;
    set_no_cache(req, "text/html");

    int count = wifi_list_count();
    app_state_t st = get_app_state();

    char *page = malloc(4096);
    if (!page) return ESP_ERR_NO_MEM;
    int w = snprintf(page, 4096,
        "<html><head><title>WiFi Networks</title>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='format-detection' content='telephone=no'>"
        "</head>"
        "<body style='font-family:sans-serif;padding:20px;text-align:center;'>"
        "<h2>WiFi Networks</h2><hr>");

    if (st == APP_WIFI_FAILED) {
        w += snprintf(page + w, 4096 - w,
            "<p style='color:#e74c3c;'>Could not connect to any saved "
            "network.<br>Check the list below.</p>");
    }

    if (count == 0) {
        w += snprintf(page + w, 4096 - w,
            "<p>No networks saved yet.<br>"
            "The gateway connects to the strongest saved network "
            "it can find.</p>");
    } else {
        w += snprintf(page + w, 4096 - w,
            "<div style='text-align:left;background:#ecf0f1;padding:15px;'>");
        for (int i = 0; i < count && w < 3500; i++) {
            char ssid[33], esc[80];
            if (!wifi_list_get_ssid(i, ssid, sizeof(ssid))) continue;
            html_escape(ssid, esc, sizeof(esc));
            w += snprintf(page + w, 4096 - w,
                "%d. <b>%s</b> [<a href='/wifi/del?i=%d'>delete</a>]<br>",
                i + 1, esc, i);
        }
        w += snprintf(page + w, 4096 - w, "</div>");
    }

    w += snprintf(page + w, 4096 - w,
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
    int rec = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rec <= 0) return ESP_FAIL;
    buf[rec] = '\0';

    char ns[64] = {0}, np[80] = {0};
    if (httpd_query_key_value(buf, "ssid", ns, sizeof(ns)) == ESP_OK) {
        httpd_query_key_value(buf, "pass", np, sizeof(np));

        /* application/x-www-form-urlencoded: '+' means space, %XX escapes */
        char ssid[64] = {0}, pass[80] = {0};
        size_t si = 0, pi = 0;
        for (char *p = ns; *p && si + 1 < sizeof(ssid); p++) {
            if (*p == '+') { ssid[si++] = ' '; continue; }
            if (*p == '%' && p[1] && p[2]) {
                char hex[3] = { p[1], p[2], 0 };
                ssid[si++] = (char)strtol(hex, NULL, 16);
                p += 2;
                continue;
            }
            ssid[si++] = *p;
        }
        for (char *p = np; *p && pi + 1 < sizeof(pass); p++) {
            if (*p == '+') { pass[pi++] = ' '; continue; }
            if (*p == '%' && p[1] && p[2]) {
                char hex[3] = { p[1], p[2], 0 };
                pass[pi++] = (char)strtol(hex, NULL, 16);
                p += 2;
                continue;
            }
            pass[pi++] = *p;
        }

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
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = 2;
    if (httpd_start(&http_server, &cfg) == ESP_OK) {
        static const httpd_uri_t uris[] = {
            { "/",            HTTP_GET,  handler_root,     NULL },
            { "/wifi",        HTTP_GET,  handler_wifi_get, NULL },
            { "/wifi/add",    HTTP_POST, handler_wifi_add, NULL },
            { "/wifi/del",    HTTP_GET,  handler_wifi_del, NULL },
            { "/reset",       HTTP_GET,  handler_reset,    NULL },
            { "/reboot",      HTTP_GET,  handler_reboot,   NULL },
            { "/favicon.ico", HTTP_GET,  handler_favicon,  NULL },
        };
        for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
            httpd_register_uri_handler(http_server, &uris[i]);
        httpd_register_err_handler(http_server,
                                   HTTPD_404_NOT_FOUND, handler_404);
        ESP_LOGI(TAG, "web UI at http://" GW_IP_STR "/");
    } else {
        ESP_LOGE(TAG, "httpd_start failed");
    }
}
