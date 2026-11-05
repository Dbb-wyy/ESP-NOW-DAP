#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dbb
#
# SPDX-License-Identifier: Apache-2.0
"""Capture the ESP-IDF boot log of an already flashed board.

Why this script exists: after ``idf.py flash`` the board is reset by the flashing
tool while the serial port is still closed, and opening it a moment later is racy -
the capture sometimes returns 0 bytes or a truncated line. This script opens the
port first, then pulses EN through RTS (keeping GPIO0 high through DTR, i.e. a
normal boot), so every capture starts at the first ROM line. It also retries the
reset pulse a couple of times if nothing arrives, which removed the leftovers of
the race in practice.

Usage:
    python3 capture_boot.py <PORT> <OUTPUT_FILE> [SECONDS]

Example:
    python3 capture_boot.py /dev/ttyACM0 build_r_host/boot_skill.log 12

The script prints a one-line summary and exits:
    0 = bytes captured (signature may still be missing: caller must inspect)
    2 = serial port could not be opened / written
"""

import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial is missing: run this through the ESP-IDF environment "
                     "(source ~/.espressif/tools/activate_idf_v6.0.2.sh)\n")
    sys.exit(2)


SIGNATURES = (
    b"Free heap after start-up:",
    b"ESP USB Bridge running as wireless host",
    b"ESP USB Bridge running as wireless slave",
)


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    port = sys.argv[1]
    out_path = sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0

    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
    except serial.SerialException as exc:
        sys.stderr.write(f"cannot open {port}: {exc}\n")
        return 2

    # GPIO0 high (normal boot, not the ROM downloader) and EN held in reset:
    # the chip must not start before the first deliberate pulse, otherwise a
    # pre-boot fragment shows up in front of the real log. Drop any bytes the
    # driver buffer still holds from a previous session too.
    ser.dtr = False
    ser.rts = True
    ser.reset_input_buffer()
    time.sleep(0.05)

    buf = bytearray()
    pulses = 0

    def pulse_reset() -> None:
        nonlocal pulses
        ser.rts = True
        time.sleep(0.15)
        ser.rts = False
        pulses += 1

    pulse_reset()
    start = time.monotonic()
    last_pulse = start

    try:
        while time.monotonic() - start < seconds:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
            elif (not buf and pulses < 3 and time.monotonic() - last_pulse > 2.5):
                # Not a single byte arrived (closed port / board in reset): pulse EN
                # once more. A board that booted and then went quiet is normal - the
                # firmware only logs during start-up - so never reset it again.
                pulse_reset()
                last_pulse = time.monotonic()
    finally:
        ser.close()

    data = bytes(buf)
    with open(out_path, "wb") as out:
        out.write(data)

    hits = [sig.decode() for sig in SIGNATURES if sig in data]
    print(f"captured {len(data)} bytes in {time.monotonic() - start:.1f}s "
          f"({pulses} reset pulse(s)) -> {out_path}")
    print("signature hits: " + (", ".join(hits) if hits else "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
