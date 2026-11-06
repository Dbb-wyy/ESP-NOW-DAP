/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/param.h>

#include "sdkconfig.h"

#include "target_uart.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#define KB(x) ((x) * 1024)

#define TARGET_UART_TASK_PRI 5

#define TARGET_UART_NUM          UART_NUM_1
#define TARGET_UART_BUF_SIZE     KB(2)
#define TARGET_UART_DEFAULT_BAUD 115200
#define GPIO_BOOT CONFIG_TARGET_UART_GPIO_BOOT
#define GPIO_RST CONFIG_TARGET_UART_GPIO_RST
#define GPIO_RXD CONFIG_TARGET_UART_GPIO_RXD
#define GPIO_TXD CONFIG_TARGET_UART_GPIO_TXD

static const char *TAG = "target_uart";

typedef struct {
    bool is_initialized;

    QueueHandle_t uart_queue;

    target_uart_rx_cb_t data_callback;
} uart_state_t;

static uart_state_t s_uart = {0};

/* UART event handling task */
static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t dtmp[TARGET_UART_BUF_SIZE];

    while (1) {
        if (xQueueReceive(s_uart.uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA:
                // Only call the callback if one is registered.
                if (s_uart.data_callback) {
                    size_t buffered_len;
                    uart_get_buffered_data_len(TARGET_UART_NUM, &buffered_len);
                    const int read = uart_read_bytes(TARGET_UART_NUM, dtmp, MIN(buffered_len, TARGET_UART_BUF_SIZE), portMAX_DELAY);
                    ESP_LOGD(TAG, "UART -> Bridge Callback (%d bytes)", read);
                    ESP_LOG_BUFFER_HEXDUMP("UART RX", dtmp, read, ESP_LOG_DEBUG);

                    s_uart.data_callback(dtmp, read);
                }
                break;
            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART FIFO overflow");
                uart_flush_input(TARGET_UART_NUM);
                xQueueReset(s_uart.uart_queue);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART ring buffer full");
                uart_flush_input(TARGET_UART_NUM);
                xQueueReset(s_uart.uart_queue);
                break;
            case UART_BREAK:
                ESP_LOGW(TAG, "UART RX break");
                break;
            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "UART parity error");
                break;
            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "UART frame error");
                break;
            default:
                ESP_LOGW(TAG, "UART event type: %d", event.type);
                break;
            }
            taskYIELD();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static esp_err_t init_uart(void)
{
    const uart_config_t uart_config = {
        .baud_rate = TARGET_UART_DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Sets pin current strength to 40 mA, because some pins default to 10 mA,
    // which is not enough when USB-UART is also present on the UART lines
    // (at least 20 mA should be sufficient).
    ESP_RETURN_ON_ERROR(gpio_set_drive_capability(GPIO_TXD, GPIO_DRIVE_CAP_3), TAG,
                        "Failed to set TXD drive capability");
    ESP_RETURN_ON_ERROR(uart_param_config(TARGET_UART_NUM, &uart_config), TAG, "Failed to configure UART");
    ESP_RETURN_ON_ERROR(uart_set_pin(TARGET_UART_NUM, GPIO_TXD, GPIO_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "Failed to set UART pins");
    ESP_RETURN_ON_ERROR(uart_driver_install(TARGET_UART_NUM, TARGET_UART_BUF_SIZE * 2, 0, 20, &s_uart.uart_queue, 0),
                        TAG, "Failed to install UART driver");

    // The target boot / reset lines start as plain outputs with pull-ups.
    gpio_reset_pin(GPIO_RST);
    gpio_set_pull_mode(GPIO_RST, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_RST, GPIO_MODE_OUTPUT);
    gpio_reset_pin(GPIO_BOOT);
    gpio_set_pull_mode(GPIO_BOOT, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_BOOT, GPIO_MODE_OUTPUT);

    // Enable pull up for RXD to avoid floating input
    ESP_RETURN_ON_ERROR(gpio_pullup_en(GPIO_RXD), TAG, "Failed to enable pull up for RXD");

    // Override BOOT/RST pins to open-drain so the bridge never actively drives them HIGH.
    // The internal pull-ups (and any external pull-ups on the target) define the idle HIGH level,
    // while other devices can still pull the lines LOW without bus contention.
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_BOOT, GPIO_MODE_OUTPUT_OD), TAG, "Failed to set boot pin to open-drain");
    ESP_RETURN_ON_ERROR(gpio_pullup_en(GPIO_BOOT), TAG, "Failed to enable pull up for BOOT");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_BOOT, 1), TAG, "Failed to release boot pin");
    ESP_RETURN_ON_ERROR(gpio_set_direction(GPIO_RST, GPIO_MODE_OUTPUT_OD), TAG, "Failed to set reset pin to open-drain");
    ESP_RETURN_ON_ERROR(gpio_pullup_en(GPIO_RST), TAG, "Failed to enable pull up for RST");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_RST, 1), TAG, "Failed to release reset pin");

    ESP_LOGI(TAG, "UART have been initialized");

    // Start UART event task
    xTaskCreate(uart_event_task, "uart_task", KB(8), NULL, TARGET_UART_TASK_PRI, NULL);

    return ESP_OK;
}

esp_err_t target_uart_init(void)
{
    if (s_uart.is_initialized) {
        ESP_LOGW(TAG, "Comm handler already initialized");
        return ESP_OK;
    }

    s_uart.data_callback = NULL;

    const esp_err_t ret = init_uart();
    if (ret == ESP_OK) {
        s_uart.is_initialized = true;
        ESP_LOGI(TAG, "Comm handler initialized");
    }

    return ret;
}

esp_err_t target_uart_register_rx_cb(target_uart_rx_cb_t callback)
{
    if (!s_uart.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_uart.data_callback = callback;
    ESP_LOGI(TAG, "Data callback registered");
    return ESP_OK;
}

esp_err_t target_uart_send(const uint8_t *data, size_t len)
{
    if (!s_uart.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Sending %zu bytes", len);
    ESP_LOG_BUFFER_HEXDUMP("UART TX", data, len, ESP_LOG_DEBUG);

    const int transferred = uart_write_bytes(TARGET_UART_NUM, data, len);
    if (transferred != len) {
        ESP_LOGW(TAG, "uart_write_bytes transferred %d bytes only!", transferred);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t target_uart_set_baudrate(uint32_t baud)
{
    if (!s_uart.is_initialized) {
        ESP_LOGE(TAG, "Comm handler not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = uart_set_baudrate(TARGET_UART_NUM, baud);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "UART baudrate set to %" PRIu32, baud);
    } else {
        ESP_LOGE(TAG, "Failed to set UART baudrate to %" PRIu32, baud);
    }
    return result;
}

void target_uart_set_boot_reset_pins(bool boot_pin, bool reset_pin)
{
    gpio_set_level(GPIO_BOOT, boot_pin);
    gpio_set_level(GPIO_RST, reset_pin);
    ESP_LOGD(TAG, "BOOT=%s, RST=%s", boot_pin ? "HIGH" : "LOW", reset_pin ? "HIGH" : "LOW");
}