/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_rgb.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led_rgb";

/*
 * The gate is CONFIG_BRIDGE_LED_ENABLE, deliberately not the pin: CONFIG_BRIDGE_WS2812_GPIO
 * depends on it, so the pin symbol is *undefined* when the LED is disabled. Testing
 * "#if CONFIG_BRIDGE_WS2812_GPIO >= 0" would then be a comparison of 0 against 0 and
 * would compile the enabled implementation anyway.
 */
#if CONFIG_BRIDGE_LED_ENABLE

#include "led_strip.h"

#define LED_RGB_PIN CONFIG_BRIDGE_WS2812_GPIO

/* 40 Hz is far above what the eye needs for a breathing LED, and the renderer only
   touches the RMT peripheral when the colour actually changes. */
#define LED_RGB_TICK_MS     25
#define LED_RGB_TASK_STACK  3072
#define LED_RGB_TASK_PRIO   2

/* How long an explicit led_rgb_set() survives before the state machine takes over. */
#define LED_RGB_MANUAL_HOLD_MS 1000

/* One rendering style per state. Only the hue is fixed here; brightness, blink rate and
   breathing are Kconfig options, because they are a matter of taste on a real board. */
typedef struct {
    const char *name;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    bool blink;     /* true: blink at CONFIG_BRIDGE_LED_BLINK_*, false: breathe */
} led_style_t;

static const led_style_t s_style_down = { "link down", 255, 180, 0, true };
static const led_style_t s_style_both = { "SWD and serial", 140, 0, 255, true };
static const led_style_t s_style_swd = { "SWD", 0, 80, 255, true };
static const led_style_t s_style_serial = { "serial", 0, 255, 60, true };
static const led_style_t s_style_idle = { "link up, idle", 0, 180, 180, false };

static led_strip_handle_t s_strip;
static int s_pin = -1;
static const led_rgb_link_ops_t *s_ops;
static const led_style_t *s_style_shown;

/* Last activity per event, in milliseconds. A call site stores one 32 bit value, which
   is atomic on both the Xtensa and the RISC-V cores used here and cannot tear; 0 means
   "never", and every comparison uses the wrap safe difference. */
static volatile uint32_t s_activity_ms[LED_RGB_ACT_MAX];

/* Automatic rendering is suspended until this timestamp after an explicit led_rgb_set(),
   so a caller asking for a colour is not fought by the state machine. 0 = no override. */
static volatile uint32_t s_manual_until_ms;

/* Last colour handed to the strip, so an unchanged frame costs nothing at all. */
static uint8_t s_shown[3];
static bool s_shown_valid;

#if LED_RGB_PIN >= 0
/* Pins the rest of the firmware drives. A collision would make the status LED fight a
   functional signal (the C3 in-package flash pins are exactly this failure), so a hit is
   reported as a configuration error instead of being driven anyway. */
static const struct {
    const char *name;
    int pin;
} s_used_pins[] = {
    { "CONFIG_SERIAL_HANDLER_GPIO_TXD", CONFIG_SERIAL_HANDLER_GPIO_TXD },
    { "CONFIG_SERIAL_HANDLER_GPIO_RXD", CONFIG_SERIAL_HANDLER_GPIO_RXD },
    { "CONFIG_SERIAL_HANDLER_GPIO_BOOT", CONFIG_SERIAL_HANDLER_GPIO_BOOT },
    { "CONFIG_SERIAL_HANDLER_GPIO_RST", CONFIG_SERIAL_HANDLER_GPIO_RST },
    { "CONFIG_DEBUG_PROBE_GPIO_TCK", CONFIG_DEBUG_PROBE_GPIO_TCK },
    { "CONFIG_DEBUG_PROBE_GPIO_TMS", CONFIG_DEBUG_PROBE_GPIO_TMS },
#if CONFIG_DEBUG_PROBE_IFACE_JTAG
    { "CONFIG_DEBUG_PROBE_GPIO_TDI", CONFIG_DEBUG_PROBE_GPIO_TDI },
    { "CONFIG_DEBUG_PROBE_GPIO_TDO", CONFIG_DEBUG_PROBE_GPIO_TDO },
#endif
};
#endif /* LED_RGB_PIN >= 0 */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool activity_recent(led_rgb_activity_t act, uint32_t now)
{
    const uint32_t last = s_activity_ms[act];

    return last != 0 && (uint32_t)(now - last) < CONFIG_BRIDGE_LED_ACT_WINDOW_MS;
}

/* 0 while the LED is off in this blink cycle, 255 while it is on. */
static uint32_t blink_level(uint32_t now)
{
    return (now % CONFIG_BRIDGE_LED_BLINK_PERIOD_MS) < CONFIG_BRIDGE_LED_BLINK_ON_MS ? 255 : 0;
}

/* Triangle wave over the configured period, mapped between the configured minimum
   brightness and full scale. */
static uint32_t breath_level(uint32_t now)
{
    const uint32_t period = CONFIG_BRIDGE_LED_BREATH_PERIOD_MS;
    const uint32_t half = period / 2;
    const uint32_t phase = now % period;

    uint32_t triangle = (phase < half) ? (phase * 255u) / half
                                       : ((period - phase) * 255u) / half;
    if (triangle > 255u) {
        triangle = 255u;    /* the rounded half can overshoot by one */
    }

    const uint32_t min_level = ((uint32_t)CONFIG_BRIDGE_LED_BREATH_MIN_PCT * 255u) / 100u;

    return min_level + (triangle * (255u - min_level)) / 255u;
}

/* Write one colour, applying the global brightness ceiling. Repeated colours are
   dropped before they reach the peripheral. */
static void led_write(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t scaled[3] = {
        (uint8_t)(((uint32_t)red * CONFIG_BRIDGE_LED_BRIGHTNESS) / 255u),
        (uint8_t)(((uint32_t)green * CONFIG_BRIDGE_LED_BRIGHTNESS) / 255u),
        (uint8_t)(((uint32_t)blue * CONFIG_BRIDGE_LED_BRIGHTNESS) / 255u),
    };

    if (s_shown_valid && s_shown[0] == scaled[0] && s_shown[1] == scaled[1] && s_shown[2] == scaled[2]) {
        return;
    }

    s_shown[0] = scaled[0];
    s_shown[1] = scaled[1];
    s_shown[2] = scaled[2];
    s_shown_valid = true;

    if (s_strip == NULL) {
        return;
    }

    if (scaled[0] == 0 && scaled[1] == 0 && scaled[2] == 0) {
        led_strip_clear(s_strip);
        return;
    }

    led_strip_set_pixel(s_strip, 0, scaled[0], scaled[1], scaled[2]);
    led_strip_refresh(s_strip);
}

static void led_render(uint32_t now)
{
    if (s_manual_until_ms != 0 && (uint32_t)(now - s_manual_until_ms) < LED_RGB_MANUAL_HOLD_MS) {
        return;     /* an explicitly requested colour is showing */
    }

    /*
     * The link is pulled, never latched here. An unregistered source (the few
     * milliseconds between led_rgb_init() and bridge_uplink_init(), or a failed
     * wlink_init()) reports "down" rather than pretending the link is fine.
     */
    const bool up = s_ops != NULL && s_ops->link_up != NULL && s_ops->link_up();
    const bool swd = activity_recent(LED_RGB_ACT_SWD, now);
    const bool serial = activity_recent(LED_RGB_ACT_SERIAL, now);

    const led_style_t *style = &s_style_idle;
    if (!up) {
        style = &s_style_down;
    } else if (swd && serial) {
        style = &s_style_both;
    } else if (swd) {
        style = &s_style_swd;
    } else if (serial) {
        style = &s_style_serial;
    }

    if (style != s_style_shown) {
        s_style_shown = style;
        ESP_LOGD(TAG, "Status: %s", style->name);
    }

    const uint32_t level = style->blink ? blink_level(now) : breath_level(now);

    led_write((uint8_t)(((uint32_t)style->red * level) / 255u),
              (uint8_t)(((uint32_t)style->green * level) / 255u),
              (uint8_t)(((uint32_t)style->blue * level) / 255u));
}

static void led_rgb_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        led_render(now_ms());
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_RGB_TICK_MS));
    }
}

esp_err_t led_rgb_init(void)
{
    const int pin = LED_RGB_PIN;

    if (pin < 0) {
        ESP_LOGI(TAG, "No WS2812 status LED configured (CONFIG_BRIDGE_WS2812_GPIO=-1), the LED stays off");
        return ESP_OK;
    }

    if (pin >= SOC_GPIO_PIN_COUNT) {
        ESP_LOGE(TAG, "CONFIG_BRIDGE_WS2812_GPIO=%d is outside the GPIO range of this chip (0..%d)",
                 pin, SOC_GPIO_PIN_COUNT - 1);
        return ESP_ERR_INVALID_ARG;
    }

#if LED_RGB_PIN >= 0
    for (size_t i = 0; i < sizeof(s_used_pins) / sizeof(s_used_pins[0]); i++) {
        if (s_used_pins[i].pin == pin) {
            ESP_LOGE(TAG, "CONFIG_BRIDGE_WS2812_GPIO=%d collides with %s",
                     pin, s_used_pins[i].name);
            return ESP_ERR_INVALID_ARG;
        }
    }
#endif

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = pin,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        /* 10 MHz gives 100 ns per tick, fine enough for the 0.4 us / 0.8 us WS2812 bit. */
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags = { .with_dma = false },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot create the RMT LED strip on GPIO%d: %s", pin, esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    s_pin = pin;
    led_strip_clear(s_strip);

    if (xTaskCreate(led_rgb_task, "led_rgb", LED_RGB_TASK_STACK, NULL, LED_RGB_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the status LED task");
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d (brightness %d)", s_pin, CONFIG_BRIDGE_LED_BRIGHTNESS);
    return ESP_OK;
}

bool led_rgb_is_available(void)
{
    return s_strip != NULL;
}

void led_rgb_set_link_ops(const led_rgb_link_ops_t *ops)
{
    s_ops = ops;
}

void led_rgb_notify_activity(led_rgb_activity_t act)
{
    if ((unsigned)act >= (unsigned)LED_RGB_ACT_MAX) {
        return;
    }

    /* | 1u keeps the stamp away from 0, which is the "never" marker. */
    s_activity_ms[act] = now_ms() | 1u;
}

esp_err_t led_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_manual_until_ms = now_ms() | 1u;
    led_write(red, green, blue);
    return ESP_OK;
}

esp_err_t led_rgb_off(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_manual_until_ms = 0;      /* hand control back to the state machine */
    led_write(0, 0, 0);
    return ESP_OK;
}

#else /* !CONFIG_BRIDGE_LED_ENABLE */

esp_err_t led_rgb_init(void)
{
    /* No pin is claimed, so there is nothing to validate and nothing to touch. */
    ESP_LOGI(TAG, "Status LED disabled (CONFIG_BRIDGE_LED_ENABLE=n), no LED pin is reserved or driven");
    return ESP_OK;
}

bool led_rgb_is_available(void)
{
    return false;
}

esp_err_t led_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    (void)red;
    (void)green;
    (void)blue;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t led_rgb_off(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_BRIDGE_LED_ENABLE */
