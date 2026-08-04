/*
 * gateway.h — shared state and cross-module interface for the
 * Vetera Bridge (Bluetooth RFCOMM/PPP → WiFi gateway). Architecture and concurrency rules
 * follow satura-bridge's pan_wifi_bridge.c:
 *
 *  - All shared flags/handles are read/written only inside
 *    taskENTER_CRITICAL(&state_mux) / taskEXIT_CRITICAL(&state_mux).
 *  - BTstack API calls only from the BTstack task (use
 *    btstack_run_loop_execute_on_main_thread from elsewhere).
 *  - lwIP pppapi_* calls never from the tcpip task itself.
 */

#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#define GW_IP0 192
#define GW_IP1 168
#define GW_IP2 7
#define GW_IP3 1
#define GW_IP_STR "192.168.7.1"
#define GW_PEER_IP3 2          /* phone gets 192.168.7.2 via IPCP */

/* Device name = BT_NAME_PREFIX + " " + a number derived from the BD_ADDR,
 * unless a custom name is saved. See bt_name.c. */
#define BT_NAME_PREFIX      "Vetera Bridge"
#define BT_NAME_MAX         32     /* buffer for the full advertised name */
#define BT_NAME_CUSTOM_MAX  24     /* longest custom name a user may set  */
#define BT_LEGACY_PIN   "0000"

typedef enum {
    APP_WAIT_BT,            /* discoverable, waiting for the phone */
    APP_NO_WIFI,            /* no WiFi networks configured */
    APP_WIFI_SCANNING,      /* scanning for a saved network */
    APP_WIFI_CONNECTING,    /* trying a scan candidate */
    APP_WIFI_FAILED,        /* all candidates/cycles exhausted */
    APP_BRIDGE,             /* PPP up + WiFi up: NAT active */
    APP_BRIDGE_NO_WIFI,     /* PPP up, WiFi lost: reconnecting forever */
} app_state_t;

extern portMUX_TYPE state_mux;

/* gateway.c */
void        set_app_state(app_state_t new_state);
app_state_t get_app_state(void);
const char *state_to_str(app_state_t st);
void        gw_safe_task_create(TaskFunction_t fn, const char *name,
                                uint32_t stack, void *arg,
                                UBaseType_t prio, TaskHandle_t *handle);
uint32_t    uptime_seconds(void);
void        bt_set_visible(bool v);
void        gw_wifi_kick_deferred(void);               /* any task */
void        gw_on_rfcomm_opened(uint16_t con_handle);  /* BTstack task */
void        gw_on_rfcomm_closed(void);                 /* BTstack task */
void        gw_on_ppp_up(void);                        /* tcpip task */
void        gw_on_ppp_down(void);                      /* tcpip task */
bool        get_bt_connected(void);
int8_t      get_bt_rssi(void);

/* ppp_link.c */
void ppp_link_init(void);       /* BTstack task, before hci_power_on */
bool ppp_link_up(void);
uint32_t ppp_link_tx_dropped(void);
void update_nat(void);          /* callable from any task */

/* sdp_lap.c */
void sdp_register_lap_and_spp(uint8_t lap_channel, uint8_t spp_channel);

/* bt_name.c — per-unit device name (derived from the BD_ADDR, or a custom
 * name kept in NVS). */
void bt_name_init(void);                /* before hci_power_on */
void bt_name_apply_from_addr(void);     /* BTstack task, at HCI_STATE_WORKING */
const char *bt_name_active_ptr(void);   /* for gap_set_local_name only —
                                         * BTstack keeps this pointer */
void bt_name_get(char *out, int out_len);         /* advertised name */
void bt_name_get_custom(char *out, int out_len);  /* override, "" if unset */
void bt_name_get_derived(char *out, int out_len); /* "" until the BT stack is up */
void bt_name_set(const char *name);     /* any task; "" clears the override */

/* bt_bootstrap.c — one-shot mRouter-registration connect into the phone
 * after a fresh pairing (see PLANNED_UPDATE.md). BTstack task only. */
void bt_bootstrap_on_pairing_complete(const uint8_t addr[6]);

/* wifi_multi.c */
#define WIFI_LIST_MAX 8
void wifi_multi_init(void);     /* netif/event/driver init + NVS load */
void wifi_connect_begin(void);  /* start a scan-and-pick cycle */
bool wifi_list_add(const char *ssid, const char *pass);
bool wifi_list_remove(int index);
void wifi_list_clear(void);
int  wifi_list_count(void);
bool wifi_list_get_ssid(int index, char *out, int out_len);
bool get_wifi_connected(void);
int  wifi_get_retries(void);
int8_t wifi_get_rssi(void);
void wifi_get_current(char *ssid_out, int ssid_len, char *ip_out, int ip_len);
void wifi_watchdog_check(void); /* called each heartbeat from watchdog */
struct esp_netif_obj;
struct esp_netif_obj *wifi_get_sta_netif(void);

/* bookmarks.c — shared bookmark list, so the phone never has to type a URL.
 * Capped low on purpose: the limit is the page a 176x208 screen can render
 * and the NVS partition shared with BTstack's link keys, not flash size. */
#define BM_MAX          24
#define BM_TITLE_MAX    32      /* incl. NUL */
#define BM_URL_MAX      160     /* incl. NUL */

typedef struct {
    char title[BM_TITLE_MAX];
    char url[BM_URL_MAX];
} bookmark_t;

void bookmarks_init(void);          /* load count; log NVS headroom */
int  bookmarks_count(void);
bool bookmarks_get(int index, bookmark_t *out);
bool bookmarks_set(int index, const char *title, const char *url);
                                    /* index < 0 appends */
bool bookmarks_remove(int index);

/* web_ui.c */
void web_ui_start(void);

/* dns_fwd.c */
void dns_fwd_start(void);
void dns_fwd_watchdog_check(void);  /* called each heartbeat from watchdog */

#endif
