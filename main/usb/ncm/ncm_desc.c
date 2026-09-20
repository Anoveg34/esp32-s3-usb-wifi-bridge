#include <stdio.h>
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
#define NCM_CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)

static tusb_desc_device_t s_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = 0x4006,
    .bcdDevice          = 0x0102,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

static const uint8_t s_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NCM_CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NET, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64,
                           EPNUM_NET_OUT, EPNUM_NET_IN, 64, CFG_TUD_NET_MTU),
};

static char s_mac_str[13];
static char s_product[32] = "ESP32-S3 USB Ethernet";
static char s_serial[12] = "SHARE001";

static const char *s_string[USB_NET_STRING_COUNT] = {
    [STRID_LANGID]       = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "Espressif",
    [STRID_PRODUCT]      = s_product,
    [STRID_SERIAL]       = s_serial,
    [STRID_INTERFACE]    = "USB net",
    [STRID_MAC]          = s_mac_str,
};

void usb_desc_fill(tinyusb_config_t *cfg, const uint8_t mac[6], bool nic)
{
    snprintf(s_mac_str, sizeof(s_mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (nic) {
        s_device.idProduct = 0x4008;
        s_device.bcdDevice = 0x0200;
        strlcpy(s_product, "ESP32-S3 Wi-Fi NIC", sizeof(s_product));
        strlcpy(s_serial, "NIC001", sizeof(s_serial));
        s_string[STRID_INTERFACE] = "Wi-Fi NIC";
    } else {
        s_device.idProduct = 0x4006;
        s_device.bcdDevice = 0x0102;
        strlcpy(s_product, "ESP32-S3 USB Ethernet", sizeof(s_product));
        strlcpy(s_serial, "SHARE001", sizeof(s_serial));
        s_string[STRID_INTERFACE] = "USB net";
    }
    cfg->descriptor.device = &s_device;
    cfg->descriptor.full_speed_config = s_fs_configuration;
    cfg->descriptor.string = s_string;
    cfg->descriptor.string_count = USB_NET_STRING_COUNT;
}
