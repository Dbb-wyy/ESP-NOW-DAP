# Wireless Bridge: SWD debugging + serial over ESP-NOW

This document describes the wireless bridge roles: a pair of ESP32-S3 boards that
tunnel **CMSIS-DAP SWD debugging** and **serial communication** between a PC and a
target MCU, both usable at the same time.

The wired behaviour of the project is unchanged.

---

## 1. Roles

The firmware implements one end of the bridge at a time. The role is selected in
`END-config -> Role configuration -> Firmware role` and therefore a **separate image is built and
flashed on each of the two boards**.

| Role | Sits next to | USB | Target pins | ESP-NOW |
|---|---|---|---|---|
| `Wired` | everything in one chip | CDC + vendor bulk + MSC | UART, SWD, BOOT/RST | unused |
| `Wireless host` | the PC | CDC (serial) + vendor bulk (CMSIS-DAP) | none | relays both streams |
| `Wireless slave` | the target board | **none** | UART, SWD (SWCLK/SWDIO), BOOT/RST | terminates both streams |

Both wireless roles require `END-config -> Role configuration -> Debug interface -> CMSIS-DAP SWD`.
The build fails with an explicit `#error` in `main/bridge_glue.c` otherwise.

On the ESP32-S3 the default role is the wireless host, so a plain `idf.py build` produces
the PC-side image; build the wired one with `sdkconfig.defaults.wired`, and the slave with
`sdkconfig.defaults.wireless_slave` (the C6/C3 default is the slave, they have no USB OTG).

---

## 2. Data paths

```
        PC                                    ESP32-S3 (host)              ESP32-S3 (slave)            target MCU
 ┌────────────────┐                     ┌──────────────────────┐      ┌──────────────────────┐    ┌────────────┐
 │ OpenOCD        │──USB bulk EP3──────▶│ eub_vendord          │      │ bridge_slave_swd_rx  │    │            │
 │ (cmsis-dap)    │◀─USB bulk EP3───────│   ↓ bridge_probe_*   │      │   ↓ DAP_ProcessCommand│   │            │
 │                │                     │ swd relay task       │      │   ↓ dedic_gpio 位打   │──▶│ SWCLK/SWDIO│
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
  empty payload), for the lifetime of the link. A frame is only accepted when its
  `role` field differs from the local role, so two hosts cannot hijack a slave.
  Beaconing is never stopped: a peer that reboots forgets us and can only rediscover
  us from a frame we send, so stopping would leave it unpaired until we happened to
  have payload for it.
* **Delivery** — payload traffic goes to unicast once the peer MAC is known, which
  gives ESP-NOW's hardware ACK and retransmission. Before that, frames go to broadcast
  and may be lost; that only affects start-up traffic, and a slave still receives them.
* **Chunking** — payloads larger than `WLINK_MAX_PAYLOAD` (246 B) are split into
  consecutive frames, so a 2 KB UART burst from the target arrives intact.
* **Channel** — fixed at start-up (`CONFIG_WLINK_CHANNEL`, default 1) instead of
  following the station configuration, so the pair works without an access point.
  **Both boards must use the same channel**; see §4 for why, how to choose one, and
  what happens when two pairs share a channel.
* Native ESP-IDF `esp_now.h` is used directly; there is no dependency on the
  `espressif/esp-now` component.

---

## 4. Channels: what they are and how to pick one

ESP-NOW borrows the 2.4 GHz Wi-Fi physical layer but performs no association, so there
is no access point to follow and the channel is a build-time setting of the firmware.

**What a channel is.** The 2.4 GHz ISM band (2400–2483.5 MHz) is sliced into chunks
about 20 MHz wide. Channel *n* is centred at `2412 + 5 * (n - 1)` MHz: channel 1 at
2412 MHz, 6 at 2437 MHz, 11 at 2462 MHz. The spacing is only 5 MHz while each channel
is 20 MHz wide, so neighbours overlap heavily; only channels five apart (25 MHz) are
genuinely separate. That is why 1/6/11 are the classic non-overlapping picks, and why
`CONFIG_WLINK_CHANNEL` stops at 1–13 (the union of what most regions allow; channel 14
is Japan-only and 802.11b-only).

**Same channel means shared air.** Every device on a channel — access points, phones,
Bluetooth, a microwave oven — competes for the same spectrum under CSMA/CA, so a busy
channel means more collisions, more retransmissions and higher latency. On this link
that shows up as `CONFIG_WLINK_SWD_TIMEOUT_MS` expiries, then as OpenOCD timeouts,
which is one more reason to keep `adapter speed` low.

**A different channel means physically unheard.** A receiver listens on one frequency;
a transmitter on another is not filtered out, it is simply not received. An ESP-NOW
frame carries no channel field, and the link does not ask "which network is this", so:

1. Both boards of a pair must use the same channel. `wireless_link.c` calls
   `esp_wifi_set_channel(CONFIG_WLINK_CHANNEL, WIFI_SECOND_CHAN_NONE)` and registers the
   peer with the same number (`.channel`), i.e. a strict 20 MHz narrowband with no 40 MHz
   secondary channel.
2. The channel is the **only** isolation this protocol has. Same channel = same air =
   one conversation. Two pairs on one channel hear each other: the receive filter only
   rejects frames whose `role` equals the local role, and after pairing it does not
   filter by MAC, so a slave can learn — and answer — the wrong host. Separation is by
   frequency, not by name, and it is not encryption either (§9).

**How to set it.** `CONFIG_WLINK_CHANNEL` is in
`END-config -> Wireless link -> Wi-Fi channel used by the bridge link`
(`main/Kconfig.projbuild`, `range 1 13`, `default 1`), and
`sdkconfig.defaults.wireless_host` / `.wireless_slave` currently pin it to 1. An
existing `sdkconfig` (or the file named by `-D SDKCONFIG`) wins over any defaults file,
so either edit that file or run `menuconfig`. For a reproducible second pair, put
`CONFIG_WLINK_CHANNEL=6` in a tiny extra defaults file and append it **last** in
`SDKCONFIG_DEFAULTS`, for example:

```
idf.py -B build_b_host -D SDKCONFIG=sdkconfig.b_host \
      -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host;sdkconfig.pair_b.defaults" \
      set-target esp32s3
```

Mind the board presets: the C6/C3 slaves do **not** use `sdkconfig.defaults.wireless_slave`
(it carries CPU1 symbols that do not exist on a single-core chip), so for them the channel
comes either from their own `sdkconfig.defaults.esp32c3` / `.esp32c6` or from an extra file
like the one above.

**Practical rules**

1. One pair = one channel number, identical on both boards.
2. Two pairs in the same room = two different channels, ideally chosen from {1, 6, 11}.
3. Verify in the log: `Link up as HOST, mac ..., channel 6`. A mismatch never pairs —
   `Peer discovered` never appears and the host repeats
   `Link is not paired yet, waiting up to 2000 ms`.
4. To choose: scan with a phone Wi-Fi app and take the quietest of 1/6/11; in an empty
   room the choice is arbitrary.
5. The channel does not change range or PHY rate. ESP-NOW transmits at 1 Mbps by default
   to favour range; the channel only decides how much interference shares the air.

---

## 5. SWD relay: ordering and duplicates

CMSIS-DAP is request/response, and ESP-NOW can drop or duplicate frames, so the
relay adds a small amount of protocol on top of the link.

**Host side** (`bridge_host_swd_relay_task` in `main/bridge_glue.c`)

* `tud_vendor` OUT data is queued (non-blocking) so the TinyUSB task is never
  blocked by link latency.
* Before sending, the relay waits up to `CONFIG_WLINK_PEER_WAIT_MS` for the link to
  pair. A board that was just reset does not know its peer yet, while the host tool
  sends its first command as soon as the USB device enumerates and gives up if that
  command goes unanswered. After the wait the request is sent anyway (as a broadcast,
  which the slave still receives) rather than being dropped.
* One request is in flight at a time (stop-and-wait): send, wait
  `CONFIG_WLINK_SWD_TIMEOUT_MS`, retransmit up to `CONFIG_WLINK_SWD_RETRIES` times.
* Every request carries a sequence number. A response is only accepted when its
  `seq` matches the request currently waiting; late responses for an abandoned
  request are dropped instead of being handed to OpenOCD out of order.
* If all retries fail, nothing is returned on the USB IN endpoint and OpenOCD
  times out and retries. A late answer is dropped by the `seq` check above.
* State changes are logged at info level. Per packet detail stays at debug level,
  because a debug session can send hundreds of DAP commands per second and the
  console is much slower than that; `CONFIG_WLINK_SWD_VERBOSE_LOG` promotes it to
  info level when the path has to be diagnosed.

**Slave side** (`bridge_slave_swd_rx` / `_rx_task` / `_tx_task` in `main/bridge_glue.c`)

* The last response is cached together with its sequence number. A retransmitted
  request whose `seq` equals the cached one is answered **from the cache without
  re-executing the DAP command**, which matters because replaying e.g. a register
  write is not idempotent.
* Accepted requests are recorded in a **FIFO of sequence numbers**
  (`s_swd_seq_fifo`, depth `BRIDGE_SWD_SEQ_FIFO_DEPTH`). A request whose `seq` is
  already in the FIFO is still in flight: it is ignored, and its answer goes out
  when the DAP engine produces it. This is what keeps the very first request
  (`seq` = 0, which used to collide with an unset "in flight" marker) from being
  mistaken for a duplicate and dropped.
* The response task pops the **oldest** pending `seq` and tags the response with
  it. Pairing is therefore by acceptance order, not by "the request currently
  being processed", so a late answer to an abandoned request can no longer be
  labelled with the next request's number.
* Accepting a request (duplicate check → push → enqueue) happens in one critical
  section, and the request queue is created before the rx callback is registered,
  so a request arriving right after pairing cannot fall into a missing queue.
* The DAP engine runs in its own task pinned to the same core that bound the
  dedicated GPIO, since `dedic_gpio` bundles are CPU-local.

---

## 6. Serial relay

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

## 7. Configuration

| Symbol | Where | Notes |
|---|---|---|
| `CONFIG_BRIDGE_ROLE_*` | `main/Kconfig.projbuild` | role of this image |
| `CONFIG_WLINK_CHANNEL` | `main/Kconfig.projbuild` | 1–13, default 1; must match on both boards, and two pairs need two different channels (§4) |
| `CONFIG_WLINK_PEER_WAIT_MS` | ditto | how long the host waits for pairing before sending anyway |
| `CONFIG_WLINK_SWD_TIMEOUT_MS` | ditto | per-attempt DAP response timeout |
| `CONFIG_WLINK_SWD_RETRIES` | ditto | request retransmissions before giving up |
| `CONFIG_WLINK_SWD_VERBOSE_LOG` | ditto | promote per-packet SWD relay logs from debug to info |
| `CONFIG_WLINK_PEER_TIMEOUT_MS` | ditto | silence after which the link counts as down (status LED only) |
| `CONFIG_WLINK_DEBUG_LINK_TEST` | ditto | debug only: slave sends `connected N` every second |
| `CONFIG_BRIDGE_LED_ENABLE` | ditto | optional WS2812 status LED, see the README |
| `CONFIG_BRIDGE_LED_SESSION_TIMEOUT_MS` | ditto | blue SWD state is opened by DAP Connect; 0 = only Disconnect closes it |
| `CONFIG_BRIDGE_LED_GAMMA_X100` | ditto | breathing curve, 220 = gamma 2.2, 100 = linear |
| `CONFIG_DEBUG_PROBE_GPIO_TCK/TMS` | `sdkconfig.defaults.esp32s3` | SWCLK / SWDIO |
| `CONFIG_SERIAL_HANDLER_GPIO_*` | `sdkconfig.defaults.esp32s3` | target UART + BOOT/RST |

Board settings live in `sdkconfig.defaults.esp32s3`, so all three role builds and
a plain `idf.py build` use the same pins, flash size and USB identity. The two other
supported slave targets carry their own preset:

* `sdkconfig.defaults.esp32c6` — SWCLK=4, SWDIO=5, TXD=7, RXD=6, BOOT=10, RST=9 (BOOT moved
  off GPIO8, which the devkit uses for its WS2812).
* `sdkconfig.defaults.esp32c3` — SWCLK=4, SWDIO=3, TXD=1, RXD=2, BOOT=6, RST=7, status LED
  disabled. That map exists because the chip gives away GPIO11 (VDD_SPI), GPIO12–17
  (in-package flash) and GPIO18/19 (USB). The console stays on UART0, which IDF also mirrors
  to the chip's own USB Serial/JTAG port, so either port shows the log.

---

## 8. Usage

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

## 9. Limitations

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
