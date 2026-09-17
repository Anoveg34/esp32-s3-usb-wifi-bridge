#include <string.h>
#include "usb_desc.h"
#include "tusb.h"
#include "class/net/net_device.h"

enum {
    ITF_NUM_NET = 0,
    ITF_NUM_NET_DATA,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_MAC
};

#define EPNUM_NET_NOTIF  0x81
#define EPNUM_NET_OUT    0x02
#define EPNUM_NET_IN     0x82
#define USB_VID          0x303A
#define RNDIS_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN)

static const tusb_desc_device_t s_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = 0x4005,
    .bcdDevice          = 0x0101,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

static const uint8_t s_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, RNDIS_CONFIG_TOTAL_LEN, 0, 100),
    TUD_RNDIS_DESCRIPTOR(ITF_NUM_NET, STRID_INTERFACE, EPNUM_NET_NOTIF, 8,
                         EPNUM_NET_OUT, EPNUM_NET_IN, 64),
};

static const char *s_string[USB_NET_STRING_COUNT] = {
    [STRID_LANGID]       = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "Espressif",
    [STRID_PRODUCT]      = "ESP32-S3 USB Ethernet",
    [STRID_SERIAL]       = "001",
    [STRID_INTERFACE]    = "RNDIS",
    [STRID_MAC]          = "",
};

void usb_desc_fill(tinyusb_config_t *cfg, const uint8_t mac[6])
{
    (void)mac;
    cfg->descriptor.device = &s_device;
    cfg->descriptor.full_speed_config = s_fs_configuration;
    cfg->descriptor.string = s_string;
    cfg->descriptor.string_count = USB_NET_STRING_COUNT;
}
