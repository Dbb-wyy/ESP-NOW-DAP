/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_led.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "status_led";

/*
 * The gate is CONFIG_BRIDGE_LED_ENABLE, deliberately not the pin:
 * CONFIG_BRIDGE_WS2812_GPIO depends on it, so the pin symbol is *undefined*
 * when the LED is disabled. Testing
 * "#if CONFIG_BRIDGE_WS2812_GPIO >= 0" would then be a comparison of 0 against
 * 0 and would compile the enabled implementation anyway.
 */
#if CONFIG_BRIDGE_LED_ENABLE

#include "led_strip.h"

#define STATUS_LED_PIN CONFIG_BRIDGE_WS2812_GPIO

/* 100 Hz is far above what the eye needs for a breathing LED, and the renderer
   only touches the RMT peripheral when the colour actually changes. The high
   rate matters for the breathing ramp: 100 steps per second instead of 40. */
#define STATUS_LED_TICK_MS 10

/* The state is sampled, never pushed. Half the activity window means an event
   can never age out between two polls; the price is at most half a window of
   latency between a change and the colour that shows it. */
#define STATUS_LED_POLL_MS (CONFIG_BRIDGE_LED_ACT_WINDOW_MS / 2)

#define STATUS_LED_TASK_STACK 3072
#define STATUS_LED_TASK_PRIO 2

/* The whole state machine. The declaration order also reads as the priority
   order of the old chain of tests, but nothing depends on the numbers. */
typedef enum {
    LED_STATUS_LINK_DOWN = 0,
    LED_STATUS_SWD_AND_SERIAL,
    LED_STATUS_SWD,
    LED_STATUS_SERIAL,
    LED_STATUS_IDLE,
    LED_STATUS_COUNT,
} led_status_t;

static const char *const k_status_name[LED_STATUS_COUNT] = {
    [LED_STATUS_LINK_DOWN] = "link down",
    [LED_STATUS_SWD_AND_SERIAL] = "SWD and serial",
    [LED_STATUS_SWD] = "SWD",
    [LED_STATUS_SERIAL] = "serial",
    [LED_STATUS_IDLE] = "idle",
};

static led_strip_handle_t s_strip;
static int s_pin = -1;
static const status_led_link_ops_t *s_ops;
/* A local (wired) bridge has no link to report: it never samples s_ops and its
   idle state is white instead of cyan/amber. Set once from the wired uplink. */
static bool s_local;
/* Set by an edge that has to be seen on the next tick rather than on the next
   poll: closing a debug session must drop the blue colour at once. */
static volatile bool s_poll_due;

/* Last activity per event, in milliseconds. A call site stores one 32 bit
   value, which is atomic on both the Xtensa and the RISC-V cores used here and
   cannot tear; 0 means "never", and every comparison uses the wrap safe
   difference. */
static volatile uint32_t s_activity_ms[STATUS_LED_ACT_MAX];

/* A CMSIS-DAP session is an edge, not traffic: DAP_Connect opens it and
   DAP_Disconnect closes it. The timestamp is refreshed by every DAP packet
   while the session is open, so the safety timeout only fires for a debugger
   that died without closing it. */
static volatile bool s_swd_session;
static volatile uint32_t s_swd_session_ms;

/* Last colour handed to the strip, so an unchanged frame costs nothing at all.
 */
static uint8_t s_shown[3];
static bool s_shown_valid;

#if STATUS_LED_PIN >= 0
/* Pins the rest of the firmware drives. A collision would make the status LED
   fight a functional signal (the C3 in-package flash pins are exactly this
   failure), so a hit is reported as a configuration error instead of being
   driven anyway. */
static const struct {
    const char *name;
    int pin;
} s_used_pins[] = {
    {"CONFIG_TARGET_UART_GPIO_TXD", CONFIG_TARGET_UART_GPIO_TXD},
    {"CONFIG_TARGET_UART_GPIO_RXD", CONFIG_TARGET_UART_GPIO_RXD},
    {"CONFIG_TARGET_UART_GPIO_BOOT", CONFIG_TARGET_UART_GPIO_BOOT},
    {"CONFIG_TARGET_UART_GPIO_RST", CONFIG_TARGET_UART_GPIO_RST},
    {"CONFIG_DEBUG_PROBE_GPIO_TCK", CONFIG_DEBUG_PROBE_GPIO_TCK},
    {"CONFIG_DEBUG_PROBE_GPIO_TMS", CONFIG_DEBUG_PROBE_GPIO_TMS},
#if CONFIG_DEBUG_PROBE_IFACE_JTAG
    {"CONFIG_DEBUG_PROBE_GPIO_TDI", CONFIG_DEBUG_PROBE_GPIO_TDI},
    {"CONFIG_DEBUG_PROBE_GPIO_TDO", CONFIG_DEBUG_PROBE_GPIO_TDO},
#endif
};
#endif /* STATUS_LED_PIN >= 0 */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool activity_recent(status_led_activity_t act, uint32_t now) {
    const uint32_t last = s_activity_ms[act];

    return last != 0 &&
           (uint32_t)(now - last) < CONFIG_BRIDGE_LED_ACT_WINDOW_MS;
}

static bool swd_session_active(uint32_t now) {
    (void)now;

    if (!s_swd_session) {
        return false;
    }

#if CONFIG_BRIDGE_LED_SESSION_TIMEOUT_MS > 0
    if ((uint32_t)(now - s_swd_session_ms) >=
        CONFIG_BRIDGE_LED_SESSION_TIMEOUT_MS) {
        /* the debugger died without sending DAP_Disconnect */
        return false;
    }
#endif

    return true;
}

/* 0 while the LED is off in this blink cycle, 255 while it is on. */
static uint32_t blink_level(uint32_t now) {
    return (now % CONFIG_BRIDGE_LED_BLINK_PERIOD_MS) <
                   CONFIG_BRIDGE_LED_BLINK_ON_MS
               ? 255
               : 0;
}

/* Triangle wave over the configured period, mapped between the configured
   minimum brightness and full scale. */
static uint32_t breath_level(uint32_t now) {
    const uint32_t period = CONFIG_BRIDGE_LED_BREATH_PERIOD_MS;
    const uint32_t half = period / 2;
    const uint32_t phase = now % period;

    uint32_t triangle = (phase < half) ? (phase * 255u) / half
                                       : ((period - phase) * 255u) / half;
    if (triangle > 255u) {
        triangle = 255u; /* the rounded half can overshoot by one */
    }

    const uint32_t min_level =
        ((uint32_t)CONFIG_BRIDGE_LED_BREATH_MIN_PCT * 255u) / 100u;

    return min_level + (triangle * (255u - min_level)) / 255u;
}

/*
 * A strip takes 8 bits per channel, but spreading a linear ramp over those 8
 * bits does not look linear: the eye is roughly a 2.2 power law, which is what
 * makes an untouched triangle wave look like it steps near the bottom and
 * stalls near the top. The table is built once in status_led_init(), before the
 * rendering task exists.
 */
static uint8_t s_gamma_lut[256];

static void build_gamma_lut(void) {
    const float gamma = (float)CONFIG_BRIDGE_LED_GAMMA_X100 / 100.0f;

    for (unsigned i = 0; i < 256; i++) {
        const float level = powf((float)i / 255.0f, gamma) * 255.0f + 0.5f;

        s_gamma_lut[i] = (uint8_t)(level > 255.0f ? 255.0f : level);
    }
}

/*
 * Temporal dithering, carried per channel in units of 1/65025 of full scale.
 *
 * The strip resolves 8 bits per channel, but the brightness ceiling only leaves
 * a handful of those codes in use: cyan 0,180,180 spans 0..45 at the default
 * brightness of 64 and 0..6 at brightness 8. Rounding the ramp to whole codes
 * therefore advances it in steps of a sixth of its range, which looks exactly
 * like an LED that refreshes two or three times a second - however often the
 * task actually runs and whatever STATUS_LED_TICK_MS says. Carrying the rounding
 * error into the next frame turns every step into a duty cycle, so the average
 * over a few frames follows the curve continuously. That average is what the
 * eye integrates, but only while the pulses are small or frequent enough: see
 * STATUS_LED_DITHER_MIN_PEAK below for the case where they are not.
 */
static uint32_t s_dither[3];

/*
 * Dithering is only worth its own artefact once a single code is small against
 * the colour's peak. At the default brightness of 64 cyan peaks at 45 codes and
 * one code is 2% of it, which nobody can see; at brightness 8 it peaks at 6 and
 * one code is 17%, so every dithered step becomes a pulse of that size, and a
 * pulse train repeating a few times per second is exactly what the eye calls
 * flicker. Below this peak the renderer rounds, because a slow step is the
 * lesser of the two artefacts. The number is the peak of the style, not the
 * level of this frame, so the choice does not change in the middle of a breath.
 */
#define STATUS_LED_DITHER_MIN_PEAK 16

/* Write one colour at the given level, applying the global brightness ceiling
   and, where it is safe, the dithering carry above. Repeated colours are still
   dropped before they reach the peripheral. */
static void led_write(uint8_t red, uint8_t green, uint8_t blue,
                      uint32_t level) {
    const uint8_t src[3] = {red, green, blue};
    /* level * brightness; 65025 = 255 * 255 is one full strip code. */
    const uint32_t gain = level * (uint32_t)CONFIG_BRIDGE_LED_BRIGHTNESS;
    const uint8_t top = src[0] > src[1] ? (src[0] > src[2] ? src[0] : src[2])
                                        : (src[1] > src[2] ? src[1] : src[2]);
    const uint32_t peak = ((uint32_t)top * CONFIG_BRIDGE_LED_BRIGHTNESS) / 255u;
    uint8_t scaled[3];

    if (peak < STATUS_LED_DITHER_MIN_PEAK) {
        /* Round: a one code pulse here would be a visible flicker. */
        for (unsigned i = 0; i < 3; i++) {
            scaled[i] = (uint8_t)(((uint32_t)src[i] * gain + 32512u) / 65025u);
            s_dither[i] = 0;
        }
    } else {
        for (unsigned i = 0; i < 3; i++) {
            s_dither[i] += (uint32_t)src[i] * gain;
            scaled[i] = (uint8_t)(s_dither[i] / 65025u);
            s_dither[i] -= (uint32_t)scaled[i] * 65025u;
        }
    }

    if (s_shown_valid && s_shown[0] == scaled[0] && s_shown[1] == scaled[1] &&
        s_shown[2] == scaled[2]) {
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

/*
 * The two ways a state is allowed to move. Everything else about the
 * appearance (brightness ceiling, rate, gamma) lives in led_write() above.
 * 0 and 255 are fixed points of the gamma table, so flashing is unaffected by
 * it and only the breathing ramp is bent.
 */
static void led_flash(uint8_t red, uint8_t green, uint8_t blue, uint32_t now) {
    led_write(red, green, blue, s_gamma_lut[blink_level(now)]);
}

static void led_breathe(uint8_t red, uint8_t green, uint8_t blue,
                        uint32_t now) {
    led_write(red, green, blue, s_gamma_lut[breath_level(now)]);
}

/* One function per state, colour and movement only, no policy. Recolouring a
   state is one line here instead of a search through a priority chain. */
static void state_link_down(uint32_t now) {
    led_flash(255, 180, 0, now); /* amber */
}

static void state_swd_and_serial(uint32_t now) {
    led_flash(140, 0, 255, now); /* violet */
}

static void state_swd(uint32_t now) { led_flash(0, 80, 255, now); /* blue */ }

static void state_serial(uint32_t now) {
    led_flash(0, 255, 0, now); /* green */
}

static void state_idle(uint32_t now) {
    led_breathe(0, 180, 180, now); /* cyan */
}

static void state_local_idle(uint32_t now) {
    led_breathe(255, 255, 255, now); /* white */
}

typedef void (*led_state_fn_t)(uint32_t now);

/* The single drawing entry point: hand it a state and it draws that state. */
static void led_apply_status(led_status_t status, uint32_t now) {
    static const led_state_fn_t k_draw[LED_STATUS_COUNT] = {
        [LED_STATUS_LINK_DOWN] = state_link_down,
        [LED_STATUS_SWD_AND_SERIAL] = state_swd_and_serial,
        [LED_STATUS_SWD] = state_swd,
        [LED_STATUS_SERIAL] = state_serial,
        [LED_STATUS_IDLE] = state_idle,
    };

    if ((unsigned)status >= (unsigned)LED_STATUS_COUNT) {
        /* Never go dark on a bad value: show the state that says something is
           wrong instead of showing nothing. */
        status = LED_STATUS_LINK_DOWN;
    }

    if (s_local && status == LED_STATUS_IDLE) {
        state_local_idle(now);
        return;
    }

    k_draw[status](now);
}

/*
 * Answers one question and changes nothing: what state is the bridge in now?
 * The link is pulled, never latched here, so an unregistered source (the few
 * milliseconds between status_led_init() and bridge_uplink_init(), or a failed
 * wlink_init()) reports "down" rather than pretending the link is fine. A local
 * (wired) bridge has no link at all and is treated as always linked.
 */
static led_status_t led_sample_status(uint32_t now) {
    const bool up =
        s_local || (s_ops != NULL && s_ops->link_up != NULL && s_ops->link_up());
    const bool swd =
        swd_session_active(now) || activity_recent(STATUS_LED_ACT_SWD, now);
    const bool serial = activity_recent(STATUS_LED_ACT_SERIAL, now);

    if (!up) {
        return LED_STATUS_LINK_DOWN;
    }

    if (swd && serial) {
        return LED_STATUS_SWD_AND_SERIAL;
    }

    if (swd) {
        return LED_STATUS_SWD;
    }

    if (serial) {
        return LED_STATUS_SERIAL;
    }

    return LED_STATUS_IDLE;
}

static void status_led_task(void *arg) {
    (void)arg;

    TickType_t wake = xTaskGetTickCount();
    /* One poll immediately, so the first frame is already the real state. */
    uint32_t last_poll = now_ms() - STATUS_LED_POLL_MS;
    led_status_t status = LED_STATUS_LINK_DOWN;

    for (;;) {
        const uint32_t now = now_ms();

        /*
         * Two cadences on purpose: one decides what the bridge is doing, one
         * animates it. A half window sample can never drive a one second
         * breathing ramp, which is why the drawing stays on the tick.
         */
        if (s_poll_due || (uint32_t)(now - last_poll) >= STATUS_LED_POLL_MS) {
            last_poll = now;
            s_poll_due = false;

            const led_status_t next = led_sample_status(now);

            if (next != status) {
                /* Start the new colour from a clean carry, so the dithering
                   error of the previous one cannot leak a stray code into its
                   first frame. */
                s_dither[0] = 0;
                s_dither[1] = 0;
                s_dither[2] = 0;
                ESP_LOGD(TAG, "Status: %s -> %s", k_status_name[status],
                         k_status_name[next]);
                status = next;
            }
        }

        led_apply_status(status, now);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(STATUS_LED_TICK_MS));
    }
}

esp_err_t status_led_init(void) {
    const int pin = STATUS_LED_PIN;

    if (pin < 0) {
        ESP_LOGI(TAG, "No WS2812 status LED configured "
                      "(CONFIG_BRIDGE_WS2812_GPIO=-1), the LED stays off");
        return ESP_OK;
    }

    if (pin >= SOC_GPIO_PIN_COUNT) {
        ESP_LOGE(TAG,
                 "CONFIG_BRIDGE_WS2812_GPIO=%d is outside the GPIO range of "
                 "this chip (0..%d)",
                 pin, SOC_GPIO_PIN_COUNT - 1);
        return ESP_ERR_INVALID_ARG;
    }

#if STATUS_LED_PIN >= 0
    for (size_t i = 0; i < sizeof(s_used_pins) / sizeof(s_used_pins[0]); i++) {
        if (s_used_pins[i].pin == pin) {
            ESP_LOGE(TAG, "CONFIG_BRIDGE_WS2812_GPIO=%d collides with %s", pin,
                     s_used_pins[i].name);
            return ESP_ERR_INVALID_ARG;
        }
    }
#endif

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = pin,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        /* 10 MHz gives 100 ns per tick, fine enough for the 0.4 us / 0.8 us
           WS2812 bit. */
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags = {.with_dma = false},
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot create the RMT LED strip on GPIO%d: %s", pin,
                 esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    s_pin = pin;
    led_strip_clear(s_strip);
    build_gamma_lut();

    if (xTaskCreate(status_led_task, "status_led", STATUS_LED_TASK_STACK, NULL,
                    STATUS_LED_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the status LED task");
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d (brightness %d, gamma x100 %d)",
             s_pin, CONFIG_BRIDGE_LED_BRIGHTNESS, CONFIG_BRIDGE_LED_GAMMA_X100);
    return ESP_OK;
}

void status_led_set_link_ops(const status_led_link_ops_t *ops) { s_ops = ops; }

void status_led_set_local(bool local) {
    s_local = local;
    s_poll_due = true; /* recolour on the next tick, not the next poll */
}

void status_led_notify_activity(status_led_activity_t act) {
    if ((unsigned)act >= (unsigned)STATUS_LED_ACT_MAX) {
        return;
    }

    /* | 1u keeps the stamp away from 0, which is the "never" marker. */
    const uint32_t stamp = now_ms() | 1u;
    s_activity_ms[act] = stamp;

    if (act == STATUS_LED_ACT_SWD && s_swd_session) {
        s_swd_session_ms = stamp; /* a session that keeps talking stays alive */
    }
}

void status_led_notify_swd_session(bool active) {
    if (active) {
        s_swd_session_ms = now_ms() | 1u;
        s_swd_session = true;
        return;
    }

    s_swd_session = false;
    /* Drop the traffic stamp as well, so the blue state ends now rather than
       one activity window later. */
    s_activity_ms[STATUS_LED_ACT_SWD] = 0;
    s_poll_due = true; /* and re-sample on the next tick, not the next poll */
}

#else /* !CONFIG_BRIDGE_LED_ENABLE */

void status_led_set_link_ops(const status_led_link_ops_t *ops) {
    /* No pin is claimed and no renderer runs, so there is nothing to feed. */
    (void)ops;
}

void status_led_set_local(bool local) { (void)local; }

void status_led_notify_activity(status_led_activity_t act) {
    (void)act;
}

void status_led_notify_swd_session(bool active) {
    (void)active;
}

esp_err_t status_led_init(void) {
    /* No pin is claimed, so there is nothing to validate and nothing to touch.
     */
    ESP_LOGI(TAG, "Status LED disabled (CONFIG_BRIDGE_LED_ENABLE=n), no LED "
                  "pin is reserved or driven");
    return ESP_OK;
}

#endif /* CONFIG_BRIDGE_LED_ENABLE */
