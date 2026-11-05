# Architecture of the END_nightly bridge

This document describes the **target** component architecture of the END_nightly bridge.
The code is migrated towards it phase by phase; see `.agent/Plan_v2.md` for the execution
plan, the current status and the acceptance gates.

Scope of this document: component boundaries, dependency rules, start-up order and interface
contracts. Build/flash commands live in the `.dsh/skills/` skills, rules for the engineer/agent
live in `.agent/Agent.md`.

Status of this document: describes the target. Phase 6 (B4) updates it to match the final code.

## 1. Roles and data flows

Three roles share one code base:

```
wired (ESP32-S3, one chip)
  PC --USB--> [ usb_device + bridge(wired) + target_uart + debug_probe ] --UART/SWD/BOOT/RST--> target

wireless host (ESP32-S3, next to the PC)
  PC --USB--> [ usb_device + bridge(host) ] <== ESP-NOW ==> [ bridge(slave) + target_uart + debug_probe ] --UART/SWD/BOOT/RST--> target
                                                                                (ESP32-C6 / ESP32-C3, no USB code)
```

Two independent payload streams share the radio link:

- **Serial**: CDC bytes + line coding (baud) + line state (DTR/RTS) travel as `SERIAL` and
  `SERIAL_CMD` frames. On the target side DTR/RTS becomes BOOT/RST; the 1200-baud magic value
  puts the bridge itself into ROM download mode.
- **Debug**: CMSIS-DAP 64-byte packets travel as `SWD` frames. The host side keeps one request
  in flight (stop-and-wait, 250 ms x 3 retries) and tracks the request sequence; the slave side
  keeps a sequence FIFO of accepted requests and answers each one exactly once.

## 2. Components and dependency direction

| Component | Owns | Must not know about |
|---|---|---|
| `main/` | start-up order, assembly, error handling | any concrete business logic |
| `components/bridge/` | the three roles (wired / host / slave), DAP session handling, DTR/RTS policy, sequence FIFO | USB registers, SWD bit-banging |
| `components/usb_device/` | USB descriptors/strings/BOS, TinyUSB task, PHY, vendor bulk pipe, CDC ring, magic-baud download mode, serial number | bridge, target chip, radio (compiled only on S2/S3 targets) |
| `components/target_uart/` | target UART (pins, baud, RX callback), open-drain BOOT/RST pins | where bytes come from (USB or radio) |
| `components/debug_probe/` | CMSIS-DAP engine and SWD bit-banging | how packets are transported |
| `components/wireless_link/` | ESP-NOW pairing, channel, frame envelope, retransmission primitives | whether the payload is serial or SWD |
| `components/status_led/` | five-state WS2812 status LED | SWD semantics (it only receives "activity happened") |

Dependency rules:

- **R-1** `main/` knows every component; no component knows `main/`.
- **R-2** `bridge` knows the body components (`usb_device`, `target_uart`, `debug_probe`,
  `wireless_link`, `status_led`). Body components never call `bridge`.
- **R-3** Body components never call each other. If two bodies must cooperate, `bridge` wires
  them (e.g. serial bytes go USB -> bridge -> target_uart).
- **R-4** The one exception is the USB side, which cannot be called from the USB task directly:
  `bridge` registers a function-pointer table with `usb_device_register_ops()` **before**
  `usb_device_start()`. `usb_device` may only call business code through that table.
- **R-5** A public header in `components/*/include/` must not include another component's
  public header, must not include `sdkconfig.h`, and must not contain `#if CONFIG_...`
  implementation. Configuration is read in the `.c` files (implementation), not in headers.
- **R-6** CMake may only branch on `IDF_TARGET` (evaluated before `CONFIG_*` exists); role and
  feature selection is done with Kconfig inside `.c` files. See `.agent/Agent.md` E6.

Only `components/bridge/` may look at `CONFIG_BRIDGE_ROLE_*`, and it must be the only place
that converts the build-time role into behaviour.

## 3. Start-up order (target)

Wired / wireless host (chips with USB OTG, currently ESP32-S3):

```
app_main()
  status_led_init()                 // best effort, failure only warns
  bridge_init()                     // role decision; builds usb_device_ops_t and registers it
  usb_device_start()                // PHY -> serial_bridge_init -> tusb_init -> TinyUSB task
                                    // mount callback inside usb_device:
                                    //     ops->probe_init(); eub_vendord_start();
  ESP_LOGI("Free heap after start-up: %u bytes", ...)
```

Wireless slave (ESP32-C6 / C3, no USB):

```
app_main()
  status_led_init()
  bridge_init()                     // creates the SWD request queue, registers link callbacks,
                                    // starts the two SWD relay tasks bound to the dedicated-GPIO core
  ESP_LOGI("Free heap after start-up: %u bytes", ...)
```

Rules:

- registration must happen before `usb_device_start()`; the mount callback runs inside the
  USB stack and may only use the registered ops table;
- the dedicated-GPIO core, task priorities and creation order of the SWD tasks are part of the
  behaviour contract (see `.agent/Plan_v2.md` §4.2), they must not be "cleaned up";
- the "free heap" line and the two role lines (`running as wireless slave/host`) stay in the
  log because the flash skill matches on them.

## 4. Interface contracts (finalised in phases 4-6)

### 4.1 `usb_device`

Header `usb_device.h` contains **no TinyUSB include**:

```c
typedef struct {
    esp_err_t (*serial_send)(const uint8_t *data, size_t len);
    esp_err_t (*serial_set_baudrate)(uint32_t baud);
    esp_err_t (*serial_set_line_state)(bool dtr, bool rts);
    esp_err_t (*probe_init)(void);
    void      (*probe_process_data)(const uint8_t *data, size_t len);
    esp_err_t (*probe_get_data_to_send)(uint8_t *buf, size_t buf_len, size_t *out_len);
    void      (*probe_free_sent_data)(void);
    esp_err_t (*probe_handle_command)(uint8_t cmd, uint16_t value,
                                      const uint8_t **out_data, size_t *out_len);
    uint32_t  (*probe_get_proto_caps)(void);
    esp_err_t (*probe_handle_esp32_tdi_bootstrapping)(bool boot);
} usb_device_ops_t;

esp_err_t usb_device_register_ops(const usb_device_ops_t *ops);  // before start
esp_err_t usb_device_start(void);
```

On chips without USB OTG (C3/C6) the component is **not compiled at all** (decision D3): its
`CMakeLists.txt` registers no sources and no `tinyusb`/`usb` dependency on those targets, so no
stub file exists. Only `wireless_slave` builds are supported there; `bridge` only references
`usb_device_start()` from its wired/host code paths.

### 4.2 `target_uart`

- `target_uart_init()`, `target_uart_send()`, `target_uart_set_baudrate()`,
  `target_uart_set_boot_reset_pins(bool boot, bool reset)`, `target_uart_register_rx_cb()`.
- It does not interpret DTR/RTS, does not call `debug_probe` and does not know about USB.
- The DTR/RTS -> BOOT/RST mapping (including the TDI bootstrapping hook and the 115200 baud
  restore) lives in `bridge`, so the body components stay independent (R-3). The orchestration
  is split into single-purpose helpers: `bridge_line_state_map()` (pure DTR/RTS -> BOOT/RST
  mapping), `bridge_line_state_defer_defaults()` (the 10 ms esptool workaround),
  `bridge_state_change_timer_cb()` (drives both pins high) and `bridge_apply_line_state()`
  (the sequencing itself).

### 4.3 `debug_probe`

- `debug_probe_init()`, `debug_probe_process_data()`, `debug_probe_get_data_to_send()`,
  `debug_probe_free_sent_data()`, `debug_probe_get_proto_caps()`,
  `debug_probe_handle_esp32_tdi_bootstrapping()`, and a vendor-command entry point with
  out-parameters (no internal struct exposed).
- Internal constants (task priority, string descriptor index, packet size, transport
  internals) are private; only the packet size may stay public together with a static assert
  against the USB bulk endpoint size.

### 4.4 `wireless_link`

- `wlink_init(wlink_role_t role)`: `role` is the **link-level identity** written into the
  beacon and unicast frames (host/slave pairing byte on the wire). It stays a parameter; the
  build-time `CONFIG_BRIDGE_ROLE_*` decision is made by `bridge`, which passes the role.
- `wlink_register_rx(wlink_channel_t channel, cb)`, `wlink_send(wlink_channel_t channel,
  uint8_t sub, uint8_t seq, const uint8_t *payload, size_t len)` where the channel enum is
  `{ SERIAL, SERIAL_CMD, SWD }`.
- `wlink_link_up()`, `wlink_peer_ready()`, `wlink_next_seq()` stay public; the frame header
  type, the envelope layout and the string helper become private.
- The wire format (4-byte header `{type, role, seq, sub}`), beacon interval, pairing and
  channel semantics do not change.

### 4.5 `status_led`

- `status_led_init()`, `status_led_notify_activity()`, `status_led_notify_swd_session()`,
  `status_led_set_link_ops()`, `status_led_set_local()`.
- The wireless roles breathe cyan/amber from the pulled link state (blue/green/violet for
  activity); the wired role declares itself local and breathes white while idle, because it
  has no link to report. All roles share the activity colours.
- The header contains no `sdkconfig.h` and no conditional implementation; when the LED is
  disabled the stubs are compiled inside `status_led.c`.

### 4.6 `bridge`

- `bridge_init()` is the only entry point that `main/` calls for business logic.
- Role implementations live in their own files (`bridge_wired.c`, `bridge_host.c`,
  `bridge_slave.c`); `bridge.c` keeps the shared code and the role decision.
- `bridge` owns the `usb_device_ops_t` table and the registration; it owns the DTR/RTS policy,
  the DAP session tracking and the slave sequence FIFO.

## 5. Build matrix

| Variant | Target | `sdkconfig.defaults` chain | USB | Purpose |
|---|---|---|---|---|
| `wired` | esp32s3 | `sdkconfig.defaults;sdkconfig.defaults.wired` | yes | single board, USB on the PC side |
| `host` | esp32s3 | `sdkconfig.defaults;sdkconfig.defaults.wireless_host` | yes | PC side of the wireless bridge |
| `slave_s3` | esp32s3 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | no | slave on a spare S3 |
| `slave_c6` | esp32c6 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | no | slave on C6 |
| `slave_c3` | esp32c3 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | no | slave on C3 |

All five produce `bridge.bin`; commands and success criteria are in the `esp-idf-build` skill. All variants share the project-root `partitions.csv`: `nvs` 24K, `phy_init` 4K and a factory app of 4032K (`0x10000`–`0x400000`), i.e. the app partition uses the whole 4 MB flash.

## 6. Invariants

The behaviour contract is listed once in `.agent/Plan_v2.md` §4 (wire protocol, DAP timing,
slave sequence FIFO, dedicated-GPIO core binding, pin defaults, Kconfig symbols/menu, build
matrix and product names). This document does not repeat it; it only explains how the target
structure preserves it.
