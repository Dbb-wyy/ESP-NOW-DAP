/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "tusb_config.h"
#include "esp_timer.h"

#include "eub_vendord.h"
#include "usb_defs.h"
#include "eub_abort.h"
#include "usb_device_priv.h"

static const char *TAG = "eub_vendor";
#define USB_BUSY_TOUT_US    100000 /* 100ms */
/* One level below the probe worker task (priority 5). */
#define EUB_VENDORD_TASK_PRI 4

typedef struct {
    uint8_t ep_in;
    uint8_t ep_out;
    TaskHandle_t usb_tx_task_handle;
    uint8_t epout_buf[EUB_VENDORD_EPSIZE];  /* Endpoint Transfer buffer */
} eub_vendord_interface_t;

static eub_vendord_interface_t s_eub_vendord_itf;
static const uint8_t s_rhport = 0;

/* A DAP packet was accepted and its answer has not been handed to the endpoint yet. */
static volatile bool s_answer_pending;
/* The probe backend reported that the answer of that packet is not coming. */
static volatile bool s_abort_requested;
/* The IN endpoint is stalled because that answer is not coming anymore. */
static volatile bool s_in_stalled;

/* The sender task polls in this granularity, so a reported failure reaches the host
   well inside a second instead of waiting for a full timeout. */
#define EUB_VENDORD_IN_POLL_MS   50
/* Backstop: give up even if the probe backend never reports the failure. */
#define EUB_VENDORD_IN_TIMEOUT_MS 2000
#define EUB_VENDORD_IN_POLL_TICKS pdMS_TO_TICKS(EUB_VENDORD_IN_POLL_MS)
#define EUB_VENDORD_IN_TIMEOUT_TICKS pdMS_TO_TICKS(EUB_VENDORD_IN_TIMEOUT_MS)

static void eub_vendord_release_in_stall(void)
{
    if (!s_in_stalled) {
        return;
    }

    s_in_stalled = false;
    usbd_edpt_clear_stall(s_rhport, s_eub_vendord_itf.ep_in);
    ESP_LOGI(TAG, "Vendor IN endpoint released, the host can read again");
}

/* Runs in the sender task, like every other usbd_edpt_*() call of this file. */
static void eub_vendord_stall_pending(void)
{
    if (s_in_stalled || !s_answer_pending) {
        return;
    }

    s_answer_pending = false;
    s_in_stalled = true;
    usbd_edpt_stall(s_rhport, s_eub_vendord_itf.ep_in);
    ESP_LOGW(TAG, "No answer for the last DAP packet, vendor IN endpoint stalled");
}

/* Called from the probe backend, not from the USB stack: only raise a flag and let the
   sender task do the endpoint work. */
void usb_device_vendor_abort_pending(void)
{
    s_abort_requested = true;
}

static void eub_vendord_init(void)
{
    ESP_LOGD(TAG, "%s", __func__);

    memset(&s_eub_vendord_itf, 0x00, sizeof(s_eub_vendord_itf));
}

static void eub_vendord_reset(uint8_t rhport)
{
    ESP_LOGD(TAG, "%s", __func__);

    s_eub_vendord_itf.ep_in = 0;
    s_eub_vendord_itf.ep_out = 0;
    s_answer_pending = false;
    s_abort_requested = false;
    s_in_stalled = false;
    /* do not reset the FreeRTOS handlers */
}

static uint16_t eub_vendord_open(uint8_t rhport, tusb_desc_interface_t const *desc_intf, uint16_t max_len)
{
    ESP_LOGD(TAG, "%s", __func__);

    TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == desc_intf->bInterfaceClass &&
              EUB_VENDORD_IFACE_SUBCLASS == desc_intf->bInterfaceSubClass &&
              EUB_VENDORD_IFACE_PROTOCOL == desc_intf->bInterfaceProtocol &&
              EUB_VENDORD_IFACE_STR_IDX == desc_intf->iInterface, 0);

    uint8_t const *p_desc = tu_desc_next(desc_intf);
    eub_vendord_interface_t *p_vendor = &s_eub_vendord_itf;

    // Open endpoint pair with usbd helper
    usbd_open_edpt_pair(rhport, p_desc, desc_intf->bNumEndpoints, TUSB_XFER_BULK, &p_vendor->ep_out, &p_vendor->ep_in);

    ESP_LOGI(TAG, "EP_OUT:0x%x EP_IN:0x%x", p_vendor->ep_out, p_vendor->ep_in);

    p_desc += desc_intf->bNumEndpoints * sizeof(tusb_desc_endpoint_t);

    // Prepare for incoming data
    if (p_vendor->ep_out) {
        usbd_edpt_xfer(rhport, p_vendor->ep_out, p_vendor->epout_buf, sizeof(p_vendor->epout_buf));
    }

    return (uint16_t)((uintptr_t)p_desc - (uintptr_t)desc_intf);
}

static const uint8_t desc_ms_os_20[] = {
    // Microsoft OS 2.0 Descriptor
    // Values are taken from https://github.com/hathach/tinyusb/tree/master/examples/device/webusb_serial
    //
    // Set header: length, type, windows version, total length
    // 0x000A = size of the header
    // MS_OS_20_SET_HEADER_DESCRIPTOR = 0x00 (descriptor type)
    // 0x06030000 = Windows version 6.3 (Windows 8.1 and later)
    // MS_OS_20_DESC_LEN = total length of all descriptors
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header
    // 0x0008 = size of the header
    // MS_OS_20_SUBSET_HEADER_CONFIGURATION = 0x01 (descriptor type)
    // 0 = configuration index (first configuration)
    // 0 = reserved
    // MS_OS_20_DESC_LEN - 0x0A = remaining length (total - header length)
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function Subset header
    // 0x0008 = size of the header
    // MS_OS_20_SUBSET_HEADER_FUNCTION = 0x02 (descriptor type)
    // ITF_NUM_VENDOR = interface number for vendor class
    // 0 = reserved
    // MS_OS_20_DESC_LEN - 0x0A - 0x08 = remaining length (total - header - config subset)
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // MS OS 2.0 Compatible ID descriptor
    // 0x0014 = size of the descriptor
    // MS_OS_20_FEATURE_COMPATBLE_ID = 0x03 (descriptor type)
    // 'WINUSB' = compatible ID string
    // 8 bytes of zeros = sub-compatible ID (not used)
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible

    // MS OS 2.0 Registry property descriptor
    // length = remaining bytes (total - all previous sections)
    // MS_OS_20_FEATURE_REG_PROPERTY = 0x04 (descriptor type)
    // 0x0007 = REG_MULTI_SZ (property data type)
    // 0x002A = 42 bytes for property name length ("DeviceInterfaceGUIDs" in UTF-16)
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    // 0x0050 = 80 bytes for property data length (GUID string in UTF-16)
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    // Property data: GUID "{80869ad8-c37f-476a-a6b4-ae241c30a473}" in UTF-16 format
    // Each character is followed by 0x00 for UTF-16 encoding
    '{', 0x00, '8', 0x00, '0', 0x00, '8', 0x00, '6', 0x00, '9', 0x00, 'a', 0x00, 'd', 0x00, '8', 0x00, '-', 0x00,
    'c', 0x00, '3', 0x00, '7', 0x00, 'f', 0x00, '-', 0x00, '4', 0x00, '7', 0x00, '6', 0x00, 'a', 0x00, '-', 0x00,
    'a', 0x00, '6', 0x00, 'b', 0x00, '4', 0x00, '-', 0x00, 'a', 0x00, 'e', 0x00, '2', 0x00, '4', 0x00, '1', 0x00,
    'c', 0x00, '3', 0x00, '0', 0x00, 'a', 0x00, '4', 0x00, '7', 0x00, '3', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

bool tud_vendor_control_xfer_cb(const uint8_t rhport, const uint8_t stage, tusb_control_request_t const *request)
{
    // nothing to with DATA & ACK stage
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:
        ESP_LOGI(TAG, "bRequest: (%d) wValue: (%d) wIndex: (%d)",
                 request->bRequest, request->wValue, request->wIndex);

        switch (request->bRequest) {

        case VENDOR_REQUEST_MICROSOFT:
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, MS_OS_20_DESC_LEN);
        default:
            const usb_device_ops_t *ops = usb_device_get_ops();
            if (ops == NULL || ops->probe_handle_command == NULL) {
                return false;
            }

            const uint8_t *response_data = NULL;
            size_t response_len = 0;
            esp_err_t err = ops->probe_handle_command(request->bRequest,
                                                      request->wValue,
                                                      &response_data,
                                                      &response_len);
            if (err != ESP_OK) {
                return false;
            }

            // Handle USB response
            if (response_len > 0) {
                return tud_control_xfer(rhport, request, (void *)response_data, response_len);
            } else {
                return tud_control_status(rhport, request);
            }
        }

    default:
        return false;
    }
}

static bool eub_endpt_transfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buf, uint16_t total_bytes)
{
    uint64_t end = esp_timer_get_time() + USB_BUSY_TOUT_US;

    while (usbd_edpt_busy(rhport, ep_addr)) {
        if (esp_timer_get_time() > end) {
            ESP_LOGE(TAG, "usbd_edpt_busy timeout!");
            return false;
        }
    }

    return usbd_edpt_xfer(s_rhport, ep_addr, buf, total_bytes);
}

static bool eub_vendord_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    TU_VERIFY(xferred_bytes > 0 && xferred_bytes <= 64, false);
    TU_VERIFY(ep_addr == s_eub_vendord_itf.ep_out || ep_addr == s_eub_vendord_itf.ep_in, false);

    const uint8_t ep_dir = tu_edpt_dir(ep_addr);

    ESP_LOGD(TAG, "%s xfer from ep:%x recvd:%" PRId32 " bytes", __func__, ep_addr, xferred_bytes);

    if (ep_dir == TUSB_DIR_IN) {
        /* nothing to do for now */
        return true;
    } else if (ep_dir == TUSB_DIR_OUT) {
        /* A new request means the host came back after an error, so let it read again. */
        eub_vendord_release_in_stall();

        const usb_device_ops_t *ops = usb_device_get_ops();
        esp_err_t process_result = (ops != NULL && ops->probe_process_data != NULL)
                                   ? ops->probe_process_data(s_eub_vendord_itf.epout_buf, xferred_bytes)
                                   : ESP_ERR_INVALID_STATE;
        if (process_result != ESP_OK) {
            ESP_LOGW(TAG, "Debug probe failed to process data: %s", esp_err_to_name(process_result));
            eub_abort();
        }

        /* From here on the host waits for exactly one answer on the IN endpoint. */
        s_answer_pending = true;

        // Prepare for next incoming data
        if (!eub_endpt_transfer(rhport, ep_addr, s_eub_vendord_itf.epout_buf, EUB_VENDORD_EPSIZE)) {
            ESP_LOGE(TAG, "USB transfer error on EP:%x", ep_addr);
            eub_abort();
        }

        return true;
    }

    return false;
}

static void usb_send_task(void *pvParameters)
{
    size_t total_bytes;
    uint32_t waited = 0;

    ESP_LOGI(TAG, "usb_send_task is ready!");

    for (;;) {
        const usb_device_ops_t *ops = usb_device_get_ops();
        if (ops == NULL || ops->probe_get_data_to_send == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t *buf_copy = ops->probe_get_data_to_send(&total_bytes, EUB_VENDORD_IN_POLL_TICKS);

        if (buf_copy == NULL) {
            if (s_abort_requested) {
                /* The probe backend knows that this answer will never come: tell the host
                   right away instead of letting its bulk IN read hang. */
                s_abort_requested = false;
                eub_vendord_stall_pending();
                continue;
            }

            waited += EUB_VENDORD_IN_POLL_MS;
            if (waited >= EUB_VENDORD_IN_TIMEOUT_MS) {
                waited = 0;
                eub_vendord_stall_pending();
            }
            continue;
        }

        waited = 0;
        s_abort_requested = false;
        eub_vendord_release_in_stall();

        if (!eub_endpt_transfer(s_rhport, s_eub_vendord_itf.ep_in, buf_copy, total_bytes)) {
            ESP_LOGE(TAG, "USB transfer error on EP:%x", s_eub_vendord_itf.ep_in);
            eub_abort();
        }

        s_answer_pending = false;
        ops->probe_free_sent_data(buf_copy);
    }

    vTaskDelete(NULL);
}

void eub_vendord_start(void)
{
    ESP_LOGD(TAG, "%s", __func__);

    static bool init = false;

    if (!init) {
        if (xTaskCreate(usb_send_task,
                        "usb_send_task",
                        4 * 1024,
                        NULL,
                        EUB_VENDORD_TASK_PRI,
                        &s_eub_vendord_itf.usb_tx_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Cannot create USB send task!");
            eub_abort();
        }

        init = true;
    }
}

const usbd_class_driver_t s_eub_vendord_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "EUB-VENDOR",
#endif
    .init = eub_vendord_init,
    .reset = eub_vendord_reset,
    .open = eub_vendord_open,
    .control_xfer_cb = tud_vendor_control_xfer_cb,
    .xfer_cb = eub_vendord_xfer_cb,
    .sof = NULL,
};

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    ESP_LOGD(TAG, "%s", __func__);

    *driver_count = 1;
    return &s_eub_vendord_driver;
}
