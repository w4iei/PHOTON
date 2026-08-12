#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#include "board_config.h"

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MIDI_OUT  0x03
#define EPNUM_MIDI_IN   0x83

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // IAD-aware composite (required for CDC alongside other classes).
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PHOTON_USB_VID,
    .idProduct = PHOTON_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 5, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

static const char *string_desc[] = {
    NULL,             // 0: language (special-cased)
    "PHOTON",         // 1: manufacturer
    "PHOTON Node",    // 2: product
    NULL,             // 3: serial (unique board id)
    "PHOTON Console", // 4: CDC interface
    "PHOTON MIDI",    // 5: MIDI interface
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[34];
    uint8_t len;

    if (index == 0) {
        desc[1] = 0x0409;  // en-US
        len = 1;
    } else if (index == 3) {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof serial);
        len = (uint8_t)strlen(serial);
        for (uint8_t i = 0; i < len; i++) {
            desc[1 + i] = serial[i];
        }
    } else if (index < sizeof string_desc / sizeof string_desc[0]) {
        const char *s = string_desc[index];
        len = (uint8_t)strlen(s);
        if (len > 32) {
            len = 32;
        }
        for (uint8_t i = 0; i < len; i++) {
            desc[1 + i] = s[i];
        }
    } else {
        return NULL;
    }

    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}
