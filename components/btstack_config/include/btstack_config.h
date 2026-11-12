// btstack_config.h for Vetera Bridge (ESP32 RFCOMM/PPP WiFi bridge)
// Derived from satura-bridge's config; BNEP/PAN replaced by RFCOMM.

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_ASSERT
#define HAVE_BTSTACK_STDIN
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_FREERTOS_INCLUDE_PREFIX
#define HAVE_FREERTOS_TASK_NOTIFICATIONS
#define HAVE_MALLOC

// ESP32 has VHCI
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL

// Logging
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

#define ENABLE_CLASSIC

// === CLASSIC ===
#ifdef ENABLE_CLASSIC

#define ENABLE_L2CAP

// RFCOMM memory knobs (informational with HAVE_MALLOC, but documents intent):
// two servers (LAP ch 3 + SPP ch 4), one phone at a time
#define MAX_NR_RFCOMM_MULTIPLEXERS 2
#define MAX_NR_RFCOMM_CHANNELS     2
#define MAX_NR_RFCOMM_SERVICES     2
#define MAX_NR_SDP_RECORDS         4

#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)

#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 20
#define HCI_HOST_SCO_PACKET_LEN 60
#define HCI_HOST_SCO_PACKET_NUM 10

// Legacy (pre-SSP) peers: Nokia etc.
#define ENABLE_CLASSIC_LEGACY_CONNECTIONS_FOR_SCO_DEMOS

// Link keys
// 16 phones remembered; ~3 NVS entries each. Past that, the key stored
// longest ago is evicted (BTstack: least recently stored, not used).
#define NVM_NUM_LINK_KEYS 16

#endif

#endif
