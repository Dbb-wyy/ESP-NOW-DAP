/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB device stack: descriptors, tinyusb callbacks and the device task.
 *
 * The bridge side (bridge_glue.c) registers a usb_device_ops_t table before
 * usb_device_start(). Until then the descriptors still answer enumeration; the
 * mount callback is the first point that needs the probe backend.
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "tusb_config.h"

#include "eub_abort.h"
#include "eub_vendord.h"
#include "serial_bridge.h"
#include "usb_defs.h"
#include "usb_device.h"
#include "usb_device_priv.h"
#include "usb_phy.h"

static const char *TAG = "usb_device";

/*
 * The probe publishes its capability string through one vendor string descriptor.
 * The index must be bigger than the last entry of string_desc_arr and must match
 * the value in openocd/esp_usb_bridge.cfg (< esp_usb_jtag_caps_descriptor 0x030A >).
 */
#define USB_DEVICE_PROBE_STR_DESC_INX 0x0A

static const usb_device_ops_t *s_ops;

const usb_device_ops_t *usb_device_get_ops(void)
{
    return s_ops;
}

esp_err_t usb_device_register_ops(const usb_device_ops_t *ops)
{
    if (ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ops = ops;
    return ESP_OK;
}

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, // at least 2.1 or 3.x for BOS
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#ifdef CFG_TUD_ENDPOINT0_SIZE
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
#else  // earlier versions have a typo in the name
    .bMaxPacketSize0 = CFG_TUD_ENDOINT0_SIZE,
#endif
    .idVendor = CONFIG_BRIDGE_USB_VID,
    .idProduct = CONFIG_BRIDGE_USB_PID,
    .bcdDevice = BCDDEVICE,     // defined in CMakeLists.txt
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

/*
    ESP usb builtin jtag subclass and protocol is 0xFF and 0x01 respectively.
    However, Tinyusb default values are 0x00.
    In order to use same protocol without tinyusb customization we are re-defining
    vendor descriptor here.
*/
// Interface number, string index, EP Out & IN address, EP size
#define TUD_VENDOR_EUB_DESCRIPTOR(_itfnum, _stridx, _epout, _epin, _epsize) \
  /* Interface */\
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, EUB_VENDORD_IFACE_SUBCLASS, EUB_VENDORD_IFACE_PROTOCOL, _stridx,\
  /* Endpoint Out */\
  7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,\
  /* Endpoint In */\
  7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

static uint8_t const desc_configuration[] = {
    // config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, 0, 100),

    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, 0x81, 8, EPNUM_CDC, 0x80 | EPNUM_CDC, CFG_TUD_CDC_EP_BUFSIZE),

    // Interface number, string index, EP Out & IN address, EP size
    TUD_VENDOR_EUB_DESCRIPTOR(ITF_NUM_VENDOR, EUB_VENDORD_IFACE_STR_IDX, EPNUM_VENDOR, 0x80 | EPNUM_VENDOR, 64),
};

#define MAC_BYTES       6

static char serial_descriptor[MAC_BYTES * 2 + 1] = {'\0'}; // 2 chars per hexnumber + '\0'

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
    CONFIG_BRIDGE_MANUFACTURER,    // 1: Manufacturer
#if CONFIG_DEBUG_PROBE_IFACE_JTAG
    CONFIG_BRIDGE_PRODUCT_NAME,    // 2: Product
#else
    "CMSIS-DAP",                   // OpenOCD expects "CMSIS-DAP" as a product name
#endif
    serial_descriptor,             // 3: Serials
    "CDC",
    CONFIG_DEBUG_PROBE_IFACE_NAME, // JTAG or CMSIS-DAP

    /* USB_DEVICE_PROBE_STR_DESC_INX 0x0A */
};

#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

// BOS Descriptor with Microsoft OS 2.0 support
static uint8_t const desc_bos[] = {
    // total length, number of device capabilities
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

    // Microsoft OS 2.0 descriptor
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT)
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    return desc_configuration;
}

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *) &descriptor_config;
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "Mounted");

    if (s_ops == NULL || s_ops->probe_init == NULL) {
        ESP_LOGE(TAG, "No bridge callbacks registered");
        eub_abort();
    }

    esp_err_t debug_result = s_ops->probe_init();
    if (debug_result != ESP_OK) {
        ESP_LOGW(TAG, "Debug probe initialization failed: %s", esp_err_to_name(debug_result));
        eub_abort();
    }

    eub_vendord_start();
}

static void init_serial_no(void)
{
    uint8_t m[MAC_BYTES] = {0};
    esp_err_t ret = esp_efuse_mac_get_default(m);

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Cannot read MAC address and set the device serial number");
        eub_abort();
    }

    snprintf(serial_descriptor, sizeof(serial_descriptor),
             "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

uint16_t const *tud_descriptor_string_cb(const uint8_t index, const uint16_t langid)
{
    static uint16_t _desc_str[32];  // Static, because it must exist long enough for transfer to complete
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == USB_DEVICE_PROBE_STR_DESC_INX) {
        if (s_ops == NULL || s_ops->probe_get_proto_caps == NULL) {
            return NULL;
        }
        chr_count = s_ops->probe_get_proto_caps(&_desc_str[1]) / 2;
    } else {
        // Convert ASCII string into UTF-16

        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        const char *str = string_desc_arr[index];

        // Cap at max char
        chr_count = strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    // first byte is length (including header), second byte is string type
    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

    return _desc_str;
}

static void tusb_device_task(void *pvParameters)
{
    while (1) {
        tud_task();
    }
    vTaskDelete(NULL);
}

void usb_device_target_data_received(const uint8_t *data, size_t len)
{
    serial_bridge_target_data_received(data, len);
}

esp_err_t usb_device_start(void)
{
    if (s_ops == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The serial number is part of the device descriptor: fill it before the PHY
       comes up so a host that enumerates immediately sees the real MAC. */
    init_serial_no();

    esp_err_t err = eub_usb_phy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = serial_bridge_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Serial bridge init failed: %s", esp_err_to_name(err));
        return err;
    }

    tusb_init();

    // dedic_gpio bundles are CPU-local; task must run on the same core that created them
    xTaskCreatePinnedToCore(tusb_device_task, "tusb_device_task", 4 * 1024, NULL, 5, NULL,
                            esp_cpu_get_core_id());
    return ESP_OK;
}
