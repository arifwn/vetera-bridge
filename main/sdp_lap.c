/*
 * sdp_lap.c — SDP records for the two RFCOMM channels.
 *
 * BTstack has no template for the deprecated LAN Access Profile (0x1102),
 * so the LAP record is hand-built with the de_* API, modeled on
 * spp_create_sdp_record() in src/classic/spp_server.c. The SPP record on a
 * second channel is cheap insurance: Mika Raento's original gnubox notes
 * had a working setup advertising SP (0x1101), and it is unknown which of
 * the two the phone's BTCOMM CSY actually searches for.
 *
 * Attribute IDs must be emitted in ascending order. The BrowseGroupList
 * (0x0005 → PublicBrowseRoot) is required: old Nokia SDP clients browse
 * rather than search, and without it the service can be invisible.
 */

#include <string.h>

#include "btstack_config.h"
#include "btstack.h"

#include "gateway.h"

static uint8_t lap_record[200];
static uint8_t spp_record[150];

static void create_lap_sdp_record(uint8_t *service, uint32_t record_handle,
                                   uint8_t channel) {
    uint8_t *attribute;
    de_create_sequence(service);

    // 0x0000 ServiceRecordHandle
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(service, DE_UINT, DE_SIZE_32, record_handle);

    // 0x0001 ServiceClassIDList -> { LAN Access Using PPP }
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute, DE_UUID, DE_SIZE_16,
                      BLUETOOTH_SERVICE_CLASS_LAN_ACCESS_USING_PPP);
    }
    de_pop_sequence(service, attribute);

    // 0x0004 ProtocolDescriptorList -> { {L2CAP}, {RFCOMM, channel} }
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST);
    attribute = de_push_sequence(service);
    {
        uint8_t *l2cap = de_push_sequence(attribute);
        {
            de_add_number(l2cap, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_L2CAP);
        }
        de_pop_sequence(attribute, l2cap);

        uint8_t *rfcomm = de_push_sequence(attribute);
        {
            de_add_number(rfcomm, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_RFCOMM);
            de_add_number(rfcomm, DE_UINT, DE_SIZE_8, channel);
        }
        de_pop_sequence(attribute, rfcomm);
    }
    de_pop_sequence(service, attribute);

    // 0x0005 BrowseGroupList -> { PublicBrowseRoot }
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_BROWSE_GROUP_LIST);
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute, DE_UUID, DE_SIZE_16, 0x1002);
    }
    de_pop_sequence(service, attribute);

    // 0x0006 LanguageBaseAttributeIDList (en, UTF-8, base 0x0100)
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_LANGUAGE_BASE_ATTRIBUTE_ID_LIST);
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute, DE_UINT, DE_SIZE_16, 0x656e);  // "en"
        de_add_number(attribute, DE_UINT, DE_SIZE_16, 0x006a);  // UTF-8
        de_add_number(attribute, DE_UINT, DE_SIZE_16, 0x0100);  // attr id base
    }
    de_pop_sequence(service, attribute);

    // 0x0008 ServiceAvailability = 0xFF (fully available)
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_SERVICE_AVAILABILITY);
    de_add_number(service, DE_UINT, DE_SIZE_8, 0xFF);

    // 0x0009 BluetoothProfileDescriptorList -> { {LAP, v1.0} }
    de_add_number(service, DE_UINT, DE_SIZE_16,
                  BLUETOOTH_ATTRIBUTE_BLUETOOTH_PROFILE_DESCRIPTOR_LIST);
    attribute = de_push_sequence(service);
    {
        uint8_t *profile = de_push_sequence(attribute);
        {
            de_add_number(profile, DE_UUID, DE_SIZE_16,
                          BLUETOOTH_SERVICE_CLASS_LAN_ACCESS_USING_PPP);
            de_add_number(profile, DE_UINT, DE_SIZE_16, 0x0100);
        }
        de_pop_sequence(attribute, profile);
    }
    de_pop_sequence(service, attribute);

    // 0x0100 ServiceName
    const char *name = "LAN Access Using PPP";
    de_add_number(service, DE_UINT, DE_SIZE_16, 0x0100);
    de_add_data(service, DE_STRING, (uint16_t)strlen(name), (uint8_t *)name);
}

void sdp_register_lap_and_spp(uint8_t lap_channel, uint8_t spp_channel) {
    memset(lap_record, 0, sizeof(lap_record));
    create_lap_sdp_record(lap_record, sdp_create_service_record_handle(),
                          lap_channel);
    btstack_assert(de_get_len(lap_record) <= sizeof(lap_record));
    sdp_register_service(lap_record);

    memset(spp_record, 0, sizeof(spp_record));
    spp_create_sdp_record(spp_record, sdp_create_service_record_handle(),
                          spp_channel, "Serial Port");
    btstack_assert(de_get_len(spp_record) <= sizeof(spp_record));
    sdp_register_service(spp_record);
}
