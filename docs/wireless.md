# Wireless Bridge: SWD debugging + serial over ESP-NOW

This document describes the wireless bridge roles: a pair of ESP32-S3 boards that
tunnel **CMSIS-DAP SWD debugging** and **serial communication** between a PC and a
target MCU, both usable at the same time.

The wired behaviour of the project is unchanged.

---

## 1. Roles

The firmware implements one end of the bridge at a time. The role is selected in
`Bridge Configuration -> Bridge role` and therefore a **separate image is built and
flashed on each of the two boards**.

| Role | Sits next to | USB | Target pins | ESP-NOW |
|---|---|---|---|---|
| `Wired` | everything in one chip | CDC + vendor bulk + MSC | UART, JTAG/SWD, BOOT/RST | unused |
| `Wireless host` | the PC | CDC (serial) + vendor bulk (CMSIS-DAP) | none | relays both streams |
| `Wireless slave` | the target board | **none** | UART, SWD (SWCLK/SWDIO), BOOT/RST | terminates both streams |

Both wireless roles require `Debug Probe Configuration -> Debug interface -> CMSIS-DAP SWD`.
The build fails with an explicit `#error` in `main/bridge_glue.c` otherwise.

---

## 2. Data paths

```
        PC                                    ESP32-S3 (host)              ESP32-S3 (slave)            target MCU
 ┌────────────────┐                     ┌──────────────────────┐      ┌──────────────────────┐    ┌────────────┐
 │ OpenOCD        │──USB bulk EP3──────▶│ eub_vendord          │      │ bridge_slave_swd_rx  │    │            │
 │ (cmsis-dap)    │◀─USB bulk EP3───────│   ↓ bridge_probe_*   │      │   ↓ DAP_ProcessCommand│   │            │
 │                │                     │ swd_relay_task       │      │   ↓ dedic_gpio 位打   │──▶│ SWCLK/SWDIO│
 │                │                     │   ⇅                  │      │                      │    │            │
 │                │                     │                      │      │                      │    │            │
 │ esptool /      │──USB CDC OUT───────▶│ tud_cdc_rx_cb        │      │ bridge_on_link_serial│    │            │
 │ 终端 / idf.py  │                     │   ↓ bridge_serial_*  │      │   ↓ uart_write_bytes │──▶│ UART RX    │
 │ monitor        │◀─USB CDC IN─────────│ usb_sender_task      │      │ uart_event_task      │◀───│ UART TX    │
 │                │                     │   ↑                  │      │   ↓ wlink_send       │    │            │
 │                │                     │                      │      │                      │    │            │
 │                │  (DTR/RTS/波特率)   │ tud_cdc_line_*_cb    │      │ bridge_on_link_sercmd│───▶│ BOOT/RST   │
 └────────────────┘                     └──────────┬───────────┘      └──────────┬───────────┘    └────────────┘
                                                   │      ESP-NOW (3 条逻辑流)     │
                                                   └──────────────────────────────┘
```

Three logical streams are multiplexed on the same ESP-NOW link:

| Stream | Direction | Payload |
|---|---|---|
| `WLINK_MSG_SERIAL` | both | raw serial bytes (chunked to 246 B per frame) |
| `WLINK_MSG_SERIAL_CMD` | host → slave | `LINE_CODING` = 4-byte baud; `LINE_STATE` = `{dtr, rts}` |
| `WLINK_MSG_SWD` | both | one CMSIS-DAP request or response, ≤ 64 B |

Because the three streams use separate message types, separate ring buffers and
separate tasks, serial traffic and SWD traffic do not block each other.

---

## 3. ESP-NOW link (`components/wireless_link`)

A fixed 4-byte header precedes every frame:

```c
typedef struct __attribute__((packed)) {
    uint8_t type;   /* wlink_msg_type_t */
    uint8_t role;   /* sender role, used to pair opposite roles only */
    uint8_t seq;    /* sequence number, used to match SWD responses */
    uint8_t sub;    /* sub type for SERIAL_CMD */
} wlink_hdr_t;
```

* **Discovery** — both sides broadcast a beacon every second (`WLINK_MSG_BEACON`,
  empty payload) until they learn the peer MAC. A frame is only accepted when its
  `role` field differs from the local role, so two hosts cannot hijack a slave.
* **Delivery** — once discovered, all traffic switches to unicast, which gives
  ESP-NOW's hardware ACK and retransmission. Before that, frames go to broadcast
  and may be lost, which only affects start-up traffic.
* **Chunking** — payloads larger than `WLINK_MAX_PAYLOAD` (246 B) are split into
  consecutive frames, so a 2 KB UART burst from the target arrives intact.
* **Channel** — fixed at start-up (`CONFIG_WLINK_CHANNEL`, default 1) instead of
  following the station configuration, so the pair works without an access point.
  **Both boards must use the same channel.**
* Native ESP-IDF `esp_now.h` is used directly; there is no dependency on the
  `espressif/esp-now` component.

---

## 4. SWD relay: ordering and duplicates

CMSIS-DAP is request/response, and ESP-NOW can drop or duplicate frames, so the
relay adds a small amount of protocol on top of the link.

**Host side** (`swd_relay_task` in `main/bridge_glue.c`)

* `tud_vendor` OUT data is queued (non-blocking) so the TinyUSB task is never
  blocked by link latency.
* One request is in flight at a time (stop-and-wait): send, wait
  `CONFIG_WLINK_SWD_TIMEOUT_MS`, retransmit up to `CONFIG_WLINK_SWD_RETRIES` times.
* Every request carries a sequence number. A response is only accepted when its
  `seq` matches the request currently waiting; late responses for an abandoned
  request are dropped instead of being handed to OpenOCD out of order.
* If all retries fail, nothing is returned on the USB IN endpoint and OpenOCD
  times out and retries. A late answer is dropped by the `seq` check above.

**Slave side** (`bridge_slave_swd_rx` in `main/bridge_glue.c`)

* The last response is cached together with its sequence number. A retransmitted
  request is answered **from the cache without re-executing the DAP command**,
  which matters because replaying e.g. a register write is not idempotent.
* A request that is already being processed is ignored; its answer is sent as
  soon as the DAP engine produces it.
* The DAP engine runs in its own task pinned to the same core that bound the
  dedicated GPIO, since `dedic_gpio` bundles are CPU-local.

---

## 5. Serial relay

* **Host**: `tud_cdc_rx_cb` forwards to the link; the link receive handler pushes
  into the existing USB sender ring buffer, so the CDC IN path is unchanged from
  wired mode. `tud_cdc_line_coding_cb` and `tud_cdc_line_state_cb` are tunnelled
  as `SERIAL_CMD` frames rather than applied locally.
* **Slave**: link `SERIAL` frames go to `uart_write_bytes`, and the UART event
  task forwards target data back over the link. `SERIAL_CMD` frames are applied
  locally: baud rate via `serial_handler_set_baudrate()`, and DTR/RTS through the
  normal BOOT/RST mapping in `bridge_apply_line_state()`.

The DTR/RTS → BOOT/RST transformation (including the esptool timing patch and the
ESP32 TDI strapping workaround) runs on the **slave**, because that is where the
reset pins are. This is what makes `esptool` work unchanged over the wireless
link: baud rate changes and the reset dance are tunneled transparently.

---

## 6. Configuration

| Symbol | Where | Notes |
|---|---|---|
| `CONFIG_BRIDGE_ROLE_*` | `main/Kconfig.projbuild` | role of this image |
| `CONFIG_WLINK_CHANNEL` | `components/wireless_link/Kconfig` | must match on both boards |
| `CONFIG_WLINK_SWD_TIMEOUT_MS` | ditto | per-attempt DAP response timeout |
| `CONFIG_WLINK_SWD_RETRIES` | ditto | request retransmissions before giving up |
| `CONFIG_DEBUG_PROBE_GPIO_TCK/TMS` | `sdkconfig.defaults.esp32s3` | SWCLK / SWDIO |
| `CONFIG_SERIAL_HANDLER_GPIO_*` | `sdkconfig.defaults.esp32s3` | target UART + BOOT/RST |

Board settings live in `sdkconfig.defaults.esp32s3`, so all three role builds and
a plain `idf.py build` use the same pins, flash size and USB identity.

---

## 7. Usage

1. Flash the **host** image to the board connected to the PC, the **slave** image
   to the board connected to the target.
2. Power both. The console prints `Peer discovered: <mac>` once the pair is up.
   The slave's log is on UART0 (it does not use the USB OTG peripheral); the
   host's log is on UART0 as well, since its native USB port is the device port.
3. **SWD** — on the PC:
   ```bash
   openocd -f interface/cmsis-dap.cfg -f target/esp32s3.cfg -c 'adapter speed 1000'
   ```
   The host advertises itself as `CMSIS-DAP` over the vendor bulk interface.
4. **Serial** — open the host's CDC port with any terminal, `idf.py monitor`, or
   `esptool`.
5. Both can be used at the same time.

---

## 8. Limitations

* The wireless roles have **no mass storage / UF2 interface**, because the host has
  no local UART to flash through. Flash the target with `esptool` over the
  tunneled serial port instead.
* ESP-NOW round-trip latency is in the millisecond range, so keep the OpenOCD
  adapter speed low (start at 1000 kHz) and expect flashing to be much slower than
  wired. Raise `CONFIG_WLINK_SWD_RETRIES` on a noisy link.
* The link is unencrypted. Anyone on the channel who implements the four-byte
  header can inject frames. Enable ESP-NOW LMK encryption if that matters.
* The host image is ~800 KB because of the Wi-Fi stack, versus ~250 KB for the
  wired image. Keep that in mind if you shrink the application partition.
