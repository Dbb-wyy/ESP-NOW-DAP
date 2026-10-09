/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "serial_bridge.h"
#include "tusb_config.h"
#include "tusb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "util.h"
#include "bridge_glue.h"
#include "serial_handler.h"
#include "usb_phy.h"
#include "soc/rtc_cntl_reg.h"

#define USB_SEND_RINGBUFFER_SIZE (2 * 1024)
/* tud_cdc_n_get_line_state(): bit 0 = DTR, bit 1 = RTS */
#define CDC_LINE_STATE_DTR (1u << 0)

static const char *TAG = "serial_bridge";

static RingbufHandle_t usb_sendbuf;
static SemaphoreHandle_t usb_tx_requested = NULL;
static SemaphoreHandle_t usb_tx_done = NULL;
static bool download_mode_armed;

/**
 * Data received from the target, e.g. from the local UART in wired mode or from the
 * wireless link in wireless host mode. Pushed into the ring buffer drained by
 * usb_sender_task(), which writes it to the USB CDC IN endpoint.
 */
void serial_bridge_target_data_received(const uint8_t *data, size_t len)
{
    /*
     * bridge_uplink_init() registers the transport receive callback (and, on the
     * wired and wireless slave roles, starts the UART event task) before
     * serial_bridge_init() creates this ring buffer, so a target that is already
     * talking can get here first. Dropping those first bytes is preferable to
     * dereferencing a NULL handle, and it keeps the initialisation order in main.c
     * from turning into a hidden requirement.
     */
    if (!usb_sendbuf) {
        ESP_LOGW(TAG, "USB sender not initialised yet, dropping %zu bytes", len);
        return;
    }

    ESP_LOGD(TAG, "Target -> USB ringbuffer (%zu bytes)", len);
    ESP_LOG_BUFFER_HEXDUMP("Target -> USB", data, len, ESP_LOG_DEBUG);

    if (xRingbufferSend(usb_sendbuf, data, len, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGV(TAG, "Cannot write to ringbuffer (free %zu of %zu)!",
                 xRingbufferGetCurFreeSize(usb_sendbuf),
                 (size_t)USB_SEND_RINGBUFFER_SIZE);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t usb_wait_for_tx(const uint32_t block_time_ms)
{
    if (xSemaphoreTake(usb_tx_done, pdMS_TO_TICKS(block_time_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void usb_sender_task(void *pvParameters)
{
    while (1) {
        size_t ringbuf_received;
        uint8_t *buf = xRingbufferReceiveUpTo(usb_sendbuf, &ringbuf_received, pdMS_TO_TICKS(100),
                                              CFG_TUD_CDC_TX_BUFSIZE);

        if (buf) {
            uint8_t int_buf[CFG_TUD_CDC_TX_BUFSIZE];
            memcpy(int_buf, buf, ringbuf_received);
            vRingbufferReturnItem(usb_sendbuf, (void *) buf);

            for (int transferred = 0, to_send = ringbuf_received; transferred < ringbuf_received;) {
                xSemaphoreGive(usb_tx_requested);
                const int wr_len = tud_cdc_write(int_buf + transferred, to_send);
                /* tinyusb might have been flushed the data. In case not flushed, we are flushing here.
                    2nd attempt might return zero, meaning there is no data to transfer. So it is safe to call it again.
                */
                tud_cdc_write_flush();
                if (usb_wait_for_tx(50) != ESP_OK) {
                    xSemaphoreTake(usb_tx_requested, 0);
                    tud_cdc_write_clear(); /* host might be disconnected. drop the buffer */
                    ESP_LOGV(TAG, "usb tx timeout");
                    break;
                }
                ESP_LOGD(TAG, "USB ringbuffer -> USB CDC (%d bytes)", wr_len);
                transferred += wr_len;
                to_send -= wr_len;
            }
        } else {
            ESP_LOGD(TAG, "usb_sender_task: nothing to send");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
    }
    vTaskDelete(NULL);
}

void tud_cdc_tx_complete_cb(const uint8_t itf)
{
    if (xSemaphoreTake(usb_tx_requested, 0) != pdTRUE) {
        /* Semaphore should have been given before write attempt.
            Sometimes tinyusb can send one more cb even xfer_complete len is zero
        */
        return;
    }

    xSemaphoreGive(usb_tx_done);
}

void tud_cdc_rx_cb(const uint8_t itf)
{
    uint8_t buf[CFG_TUD_CDC_RX_BUFSIZE];

    const uint32_t rx_size = tud_cdc_n_read(itf, buf, CFG_TUD_CDC_RX_BUFSIZE);
    if (rx_size > 0) {
        ESP_LOGD(TAG, "USB CDC -> target (%" PRIu32 " bytes)", rx_size);
        ESP_LOG_BUFFER_HEXDUMP("USB CDC -> target", buf, rx_size, ESP_LOG_DEBUG);

        /* Wired mode: straight to the target UART.
           Wireless host mode: tunnelled to the slave over ESP-NOW. */
        bridge_serial_send(buf, rx_size);
    } else {
        ESP_LOGW(TAG, "tud_cdc_rx_cb receive error");
    }
}

static void enter_download_mode(void);

void tud_cdc_line_coding_cb(const uint8_t itf, cdc_line_coding_t const *p_line_coding)
{
    if (CONFIG_BRIDGE_DOWNLOAD_MAGIC_BAUD > 0 && p_line_coding->bit_rate == CONFIG_BRIDGE_DOWNLOAD_MAGIC_BAUD) {
        ESP_LOGI(TAG, "Magic baud %" PRIu32 " detected, arming download mode reset",
                 p_line_coding->bit_rate);
        download_mode_armed = true;
        // Fire immediately if the host already cleared DTR (e.g. baud change on a closed port).
        if (!(tud_cdc_n_get_line_state(itf) & CDC_LINE_STATE_DTR)) {
            enter_download_mode();
        }
        return;
    }

    download_mode_armed = false;
    if (bridge_serial_set_baudrate(p_line_coding->bit_rate) != ESP_OK) {
        ESP_LOGE(TAG, "Could not set the baudrate to %" PRIu32, p_line_coding->bit_rate);
        eub_abort();
    }
}

void tud_cdc_line_state_cb(const uint8_t itf, const bool dtr, const bool rts)
{
    // Magic baud arms download mode; clearing DTR (typically on port close) fires it.
    if (download_mode_armed && !dtr) {
        enter_download_mode();
        return;
    }

    /* In wired mode this drives the target BOOT/RST pins directly, in wireless host
       mode the levels are forwarded to the slave, which owns those pins. */
    if (bridge_serial_set_line_state(dtr, rts) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot apply DTR = %d, RTS = %d", dtr, rts);
        eub_abort();
    }
}

static void enter_download_mode(void)
{
    if (CONFIG_BRIDGE_DOWNLOAD_MAGIC_BAUD <= 0) {
        return;
    }

    ESP_LOGI(TAG, "Forcing download mode and resetting bridge chip");
    // Tear down USB before restart; avoid logging after this point (PHY/console go away).
    // Called from tusb_device_task (CDC callbacks); esp_restart() never returns to tud_task.
    eub_usb_phy_deinit();
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

esp_err_t serial_bridge_init(void)
{
    // Create ring buffer for USB sending
    usb_sendbuf = xRingbufferCreate(USB_SEND_RINGBUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!usb_sendbuf) {
        ESP_LOGE(TAG, "Cannot create ringbuffer for USB sender");
        return ESP_ERR_NO_MEM;
    }

    // Create semaphores for USB TX synchronization
    usb_tx_done = xSemaphoreCreateBinary();
    usb_tx_requested = xSemaphoreCreateBinary();
    if (!usb_tx_done || !usb_tx_requested) {
        ESP_LOGE(TAG, "Cannot create USB TX semaphores");
        return ESP_ERR_NO_MEM;
    }

    // The transport that feeds this bridge is registered by bridge_uplink_init().

    // Start USB sender task
    xTaskCreate(usb_sender_task, "usb_sender_task", 4 * 1024, NULL, SERIAL_HANDLER_TASK_PRI, NULL);

    ESP_LOGI(TAG, "Serial bridge initialized");
    return ESP_OK;
}
