#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dbb
#
# SPDX-License-Identifier: Apache-2.0
"""Find the board on a serial port, work out which variant it runs, flash it.

Why this exists: the bridge has five build variants (Wired / wireless host /
slave on S3, C6 or C3) and the two wireless boards are the same chip, so a port
name tells you nothing: ``/dev/ttyACM0`` is whatever enumerated first today, and
``/dev/ttyACM1`` was a different board yesterday. Flashing the host image onto the
slave board costs a re-flash at best and a confused debugging session at worst.

So the identity is always derived from the hardware, never from the port name:

* the port list comes from ``/dev/serial/by-id`` (stable across re-enumeration),
* the chip is read with ``esptool flash-id`` (read-only: it only queries the ROM),
* the variant follows from chip + MAC through ``tools/boards.json``, and is
  cross-checked against the role compiled into ``build_r_<variant>``,
* nothing is written until the table below has been printed and confirmed.

Usage
-----
    tools/flash.py --list                  # ports only, no probing, no reset
    tools/flash.py                         # probe every port, print the table
    tools/flash.py --variant host          # probe, then flash that variant
    tools/flash.py --variant slave_s3 --yes
    tools/flash.py --variant wired --dry-run
    tools/flash.py --learn                 # after a flash, record MAC -> variant

Exit codes: 0 done, 1 failed, 2 nothing to do / needs a decision from the user.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

IDF_ACTIVATE = Path.home() / ".espressif" / "tools" / "activate_idf_v6.0.2.sh"
IDF_VENV = Path.home() / ".espressif" / "tools" / "python" / "v6.0.2" / "venv" / "bin"
CAPTURE = REPO / ".dsh" / "skills" / "esp-idf-flash" / "capture_boot.py"
BOARD_MAP = REPO / "tools" / "boards.json"

VARIANTS = ("wired", "host", "slave_s3", "slave_c6", "slave_c3")

# Chip each variant is built for, and the role it must report in its own config.
VARIANT_CHIP = {
    "wired": "esp32s3",
    "host": "esp32s3",
    "slave_s3": "esp32s3",
    "slave_c6": "esp32c6",
    "slave_c3": "esp32c3",
}
VARIANT_ROLE = {
    "wired": "CONFIG_BRIDGE_ROLE_WIRED",
    "host": "CONFIG_BRIDGE_ROLE_WIRELESS_HOST",
    "slave_s3": "CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE",
    "slave_c6": "CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE",
    "slave_c3": "CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE",
}

# Boot log must contain all of these, and none of the failure patterns.
ANCHORS = {
    "wired": ["Serial bridge initialized", "SWD debug probe", "Free heap after start-up"],
    "host": [
        "Link up as HOST",
        "ESP USB Bridge running as wireless host",
        "SWD relay ready",
        "usb_send_task is ready!",
        "Free heap after start-up",
    ],
    "slave_s3": ["Link up as SLAVE", "Wireless slave ready", "Free heap after start-up"],
    "slave_c6": ["Link up as SLAVE", "Wireless slave ready", "Free heap after start-up"],
    "slave_c3": ["Link up as SLAVE", "Wireless slave ready", "Free heap after start-up"],
}
FAILURES = ("Guru Meditation", "abort() was called", "Stack canary", "Brownout", "Backtrace:")

CHIP_NAMES = {
    "esp32s3": "ESP32-S3",
    "esp32c6": "ESP32-C6",
    "esp32c3": "ESP32-C3",
}


# --------------------------------------------------------------------------- #


def known_serial_devices(include_dap: bool = False) -> list[tuple[str, Path]]:
    """(by-id name, device) pairs, plus plain ttyACM/ttyUSB when by-id is empty.

    Two entries can belong to one board: the CH340 console (``usb-1a86_...``) and the
    ESP32-S3 native USB (``...CMSIS-DAP_<serial>-if00``). The native one disappears
    while the other is used for the ROM loader, so it is skipped unless asked for:
    flashing through the console port is the path this repo documents.
    """
    out: list[tuple[str, Path]] = []
    by_id = Path("/dev/serial/by-id")
    if by_id.is_dir():
        for entry in sorted(by_id.iterdir()):
            if not entry.exists():  # dangling symlink (board re-enumerated)
                continue
            if not include_dap and "CMSIS-DAP" in entry.name:
                continue
            out.append((entry.name, entry))
    if not out:
        for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
            for entry in sorted(Path("/").glob(pattern.lstrip("/"))):
                out.append((entry.name, entry))
    return out


def port_rank(name: str) -> int:
    """Lower is better: the CH340 console port flashes faster than USB-Serial-JTAG."""
    if name.startswith("usb-1a86_"):
        return 0
    if "CMSIS-DAP" in name:
        return 2
    return 1


def dedupe_by_mac(rows: list[dict]) -> list[dict]:
    """One board can expose two ports (CH340 console + native USB). Keep the best one."""
    kept: list[dict] = []
    for row in rows:
        if not row.get("ok") or not row.get("mac"):
            kept.append(row)
            continue
        same = next((k for k in kept if k.get("mac") == row["mac"]), None)
        if same is None:
            kept.append(row)
            continue
        same.setdefault("alt_ports", []).append(row["dev"])
        if port_rank(row["name"]) < port_rank(same["name"]):
            row.setdefault("alt_ports", []).extend(same.get("alt_ports", []))
            row["alt_ports"].append(same["dev"])
            kept[kept.index(same)] = row
    return kept


def esptool_path() -> str | None:
    candidate = IDF_VENV / "esptool"
    if candidate.exists():
        return str(candidate)
    return shutil.which("esptool") or shutil.which("esptool.py")


def probe(port: Path, timeout: float = 25.0) -> dict:
    """Ask the ROM who the board is. Read-only: flash-id reads the chip id."""
    tool = esptool_path()
    if tool is None:
        return {"ok": False, "error": "esptool not found (IDF virtualenv missing?)"}

    cmd = [tool, "--port", str(port), "flash-id"]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, cwd=REPO
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"no answer within {timeout:.0f} s"}

    text = proc.stdout + proc.stderr
    chip = None
    mac = None
    for line in text.splitlines():
        lowered = line.lower()
        if "chip type:" in lowered:
            match = re.search(r"chip type:\s*(ESP32-\w+)", line, re.IGNORECASE)
            if match:
                # "ESP32-S3" -> "esp32s3", the spelling the IDF uses in sdkconfig
                chip = match.group(1).lower().replace("-", "")
        elif lowered.strip().startswith("mac:"):
            match = re.search(r"([0-9a-f]{2}(?::[0-9a-f]{2}){5})", line, re.IGNORECASE)
            if match:
                mac = match.group(1).lower()

    if proc.returncode != 0 or chip is None:
        tail = [line for line in text.splitlines() if line.strip()][-3:]
        error = " / ".join(tail) or f"rc={proc.returncode}"
        gone = "No such file" in error or "couldn't open port" in error.lower()
        return {"ok": False, "error": error, "gone": gone}
    return {"ok": True, "chip": chip, "mac": mac, "raw": text}


def load_map() -> dict[str, str]:
    if not BOARD_MAP.exists():
        return {}
    try:
        data = json.loads(BOARD_MAP.read_text())
    except (OSError, ValueError):
        return {}
    boards = data.get("boards", data)
    return {k.lower(): v for k, v in boards.items() if v in VARIANTS}


def save_map(boards: dict[str, str]) -> None:
    payload = {
        "_comment": "MAC -> variant, learned by tools/flash.py --learn",
        "boards": dict(sorted(boards.items())),
    }
    BOARD_MAP.write_text(json.dumps(payload, indent=2) + "\n")


# --------------------------------------------------------------------------- #


def newest_source_mtime() -> float:
    newest = 0.0
    for pattern in ("main/**/*", "components/**/*", "sdkconfig.defaults*", "partitions.csv"):
        for path in REPO.glob(pattern):
            if path.is_file():
                newest = max(newest, path.stat().st_mtime)
    return newest


def build_info(variant: str) -> dict:
    build = REPO / f"build_r_{variant}"
    bin_path = build / "bridge.bin"
    flash_args = build / "flash_args"
    cfg = build / "config" / "sdkconfig.json"

    info = {
        "variant": variant,
        "build": build,
        "bin": bin_path,
        "flash_args": flash_args,
        "errors": [],
    }
    if not build.is_dir():
        info["errors"].append(f"{build.relative_to(REPO)} does not exist (build it first)")
        return info
    if not bin_path.exists() or not flash_args.exists():
        info["errors"].append(f"{build.relative_to(REPO)} has no bridge.bin/flash_args")
        return info

    bin_mtime = bin_path.stat().st_mtime
    src_mtime = newest_source_mtime()
    info["stale"] = bin_mtime < src_mtime
    if info["stale"]:
        info["errors"].append(
            "bridge.bin is older than the sources: rebuild before flashing"
        )

    role_ok = False
    target = None
    if cfg.exists():
        try:
            data = json.loads(cfg.read_text())
        except ValueError:
            data = {}
        # `idf.py` writes sdkconfig.json without the CONFIG_ prefix and with real
        # booleans; older tooling keeps the prefix, so accept both spellings.
        role_key = VARIANT_ROLE[variant]
        role_ok = bool(
            data.get(role_key, data.get(role_key.removeprefix("CONFIG_"), False))
        )
        target = data.get("CONFIG_IDF_TARGET") or data.get("IDF_TARGET")
        if not role_ok:
            enabled = [k for k, v in data.items() if k.startswith("CONFIG_BRIDGE_ROLE_") and v]
            info["errors"].append(
                f"build_r_{variant} was built as {enabled or 'unknown role'}, "
                f"not {VARIANT_ROLE[variant]}"
            )
        if target and target != VARIANT_CHIP[variant]:
            info["errors"].append(
                f"build_r_{variant} targets {target}, expected {VARIANT_CHIP[variant]}"
            )
    else:
        info["errors"].append(f"{cfg.relative_to(REPO)} missing: cannot verify role/target")

    info["target"] = target
    info["role_ok"] = role_ok
    info["size"] = bin_path.stat().st_size
    info["bin_mtime"] = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(bin_mtime))
    return info


def idf_env() -> dict:
    env = dict(os.environ)
    if IDF_ACTIVATE.exists():
        env["_IDF_ACTIVATE"] = str(IDF_ACTIVATE)
    return env


def idf_flash(variant: str, port: Path) -> int:
    """Flash through idf.py (the skill forbids hand-written esptool addresses)."""
    activate = f"source {IDF_ACTIVATE} >/dev/null 2>&1"
    cmd = (
        f"{activate}; cd {REPO}; idf.py -B build_r_{variant} -p {port} flash"
    )
    print(f"\n$ bash -c '{activate}; idf.py -B build_r_{variant} -p {port} flash'")
    return subprocess.call(["bash", "-c", cmd], env=idf_env())


def capture_boot(port: Path, out: Path, seconds: int) -> str:
    if not CAPTURE.exists():
        return ""
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.call(
        [sys.executable, str(CAPTURE), str(port), str(out), str(seconds)],
        cwd=REPO,
    )
    try:
        return out.read_text(errors="replace")
    except OSError:
        return ""


def check_boot(variant: str, log: str) -> tuple[list[str], list[str]]:
    missing = [anchor for anchor in ANCHORS[variant] if anchor not in log]
    failures = [pattern for pattern in FAILURES if pattern in log]
    return missing, failures


# --------------------------------------------------------------------------- #


def render_table(rows: list[dict]) -> str:
    lines = []
    header = f"{'port (by-id)':52} {'chip':10} {'mac':18} {'variant':10} {'build':16}"
    lines.append(header)
    lines.append("-" * len(header))
    for row in rows:
        if not row.get("ok"):
            name = row["name"]
            lines.append(f"{name[:52]:52} {'?':10} {'?':18} {'-':10} {'-':16}")
            continue
        name = row["name"]
        if len(name) > 50:
            name = name[:24] + "…" + name[-24:]
        lines.append(
            f"{name:52} {row.get('chip', '?') or '?':10} "
            f"{row.get('mac', '?') or '?':18} {row.get('variant', '?') or '-':10} "
            f"{('build_r_' + row['variant']) if row.get('variant') in VARIANTS else '-':16}"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Detect the board on a serial port and flash the matching variant.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--list", action="store_true", help="only list serial ports")
    parser.add_argument(
        "--include-dap",
        action="store_true",
        help="also probe the native USB-Serial-JTAG port of an ESP32-S3",
    )
    parser.add_argument("--probe", action="store_true", help="probe chips without flashing")
    parser.add_argument("--variant", choices=VARIANTS, help="variant to flash")
    parser.add_argument(
        "--port", help="by-id name or path (default: detect from --variant)"
    )
    parser.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    parser.add_argument(
        "--force",
        action="store_true",
        help="flash even though the MAC is recorded as another variant",
    )
    parser.add_argument("--dry-run", action="store_true", help="print the plan only")
    parser.add_argument("--learn", action="store_true", help="record MAC -> variant in tools/boards.json")
    parser.add_argument("--capture", type=int, default=8, help="boot log seconds (default 8)")
    args = parser.parse_args()

    devices = known_serial_devices(include_dap=args.include_dap)
    if not devices:
        print("No serial port found (is the board plugged in?)", file=sys.stderr)
        return 2

    if args.list:
        print(f"{'by-id name':72} {'device'}")
        print("-" * 96)
        for name, dev in devices:
            print(f"{name:72} {dev}")
        print("\n(--list does not probe: probing resets the board into the ROM loader)")
        return 0

    wanted = Path(args.port) if args.port else None
    if wanted is not None and args.port and not args.port.startswith("/"):
        matches = [dev for name, dev in devices if args.port in name]
        if len(matches) != 1:
            print(f"--port {args.port!r} matched {len(matches)} devices, be more specific", file=sys.stderr)
            return 2
        wanted = matches[0]
    if wanted is None and args.variant:
        # Narrow by chip first so only the plausible port is probed.
        candidates = [
            (name, dev)
            for name, dev in devices
            if VARIANT_CHIP[args.variant].lower() in name.lower()
            or VARIANT_CHIP[args.variant] == "esp32s3"  # S3 boards sit behind a CH340 or the native USB
        ]
    else:
        candidates = devices

    boards = load_map()
    rows: list[dict] = []

    if not candidates:
        print(
            f"No port looks like {CHIP_NAMES[VARIANT_CHIP[args.variant]]} — plug the board in, "
            f"or pass --port <by-id name> (see --list).",
            file=sys.stderr,
        )
        return 2

    print(f"Probing {len(candidates)} port(s) with esptool flash-id (the board resets into the ROM loader).\n")
    for name, dev in candidates:
        if wanted is not None and dev != wanted:
            continue
        result = probe(dev)
        row = {"name": name, "dev": dev, **result}
        if result.get("ok"):
            row["variant"] = boards.get(result.get("mac") or "")
        rows.append(row)
        if result.get("gone"):
            print(f"  {name}\n    -> gone (re-enumerated while probing), skipped")
            rows.pop()
            continue
        status = "ok" if result.get("ok") else f"FAILED: {result.get('error')}"
        print(f"  {name}\n    -> {row.get('chip', '?')} {row.get('mac', '?')}  [{status}]")

    if not rows:
        print("No board answered on the selected port.", file=sys.stderr)
        return 2

    rows = dedupe_by_mac(rows)

    if not args.variant:
        print("\n" + render_table(rows))
        unknown = [r for r in rows if not r.get("variant")]
        if unknown:
            print(
                "\nUnknown board(s): add them to tools/boards.json or pass --variant + --port.\n"
                "Known map: " + (", ".join(f"{m}={v}" for m, v in boards.items()) or "(empty)")
            )
        if args.probe:
            return 0
        print("\nNothing flashed: pass --variant <wired|host|slave_s3|slave_c6|slave_c3>.")
        return 0 if not unknown else 2

    variant = args.variant
    chip_needed = VARIANT_CHIP[variant]
    matching = [r for r in rows if r.get("ok") and r.get("chip") == chip_needed]
    if not matching:
        print(f"\nNo {CHIP_NAMES[chip_needed]} board found on the probed ports.", file=sys.stderr)
        return 2

    if wanted is not None:
        board = next(r for r in matching if r["dev"] == wanted)
    elif len(matching) > 1:
        with_map = [r for r in matching if r.get("variant") == variant]
        if len(with_map) == 1:
            board = with_map[0]
        else:
            print(
                f"\n{len(matching)} boards match {CHIP_NAMES[chip_needed]} and none is mapped to "
                f"{variant}: use --port, or --learn after choosing.",
                file=sys.stderr,
            )
            print(render_table(matching))
            return 2
    else:
        board = matching[0]

    info = build_info(variant)
    print("\n=== flash plan ===")
    print(f"port      : {board['name']}")
    print(f"           {board['dev']}")
    for alt in board.get("alt_ports", []):
        print(f"           (same board also on {alt})")
    print(f"chip/mac  : {board.get('chip')} / {board.get('mac')}")
    print(f"variant   : {variant}  (build_r_{variant})")
    print(f"image     : bridge.bin {info.get('size', '?')} bytes @ {info.get('bin_mtime', '?')}")
    for line in info.get("flash_args", Path("/dev/null")).read_text().splitlines()[:4] if info.get("flash_args") else []:
        print(f"            {line}")

    if info["errors"]:
        print("\nrefusing to flash:")
        for error in info["errors"]:
            print(f"  - {error}")
        return 1

    # The whole point of the map is to catch "right chip, wrong board": a board that
    # is recorded as one variant must not silently receive another one. Only an
    # explicit --port (the user named this board) or --force overrides it.
    recorded = boards.get(board.get("mac") or "")
    if recorded and recorded != variant:
        if wanted is None and not args.force:
            print(
                f"\nrefusing to flash: {board.get('mac')} is recorded as {recorded} in "
                f"{BOARD_MAP.relative_to(REPO)}, but this is a {variant} build.\n"
                f"  - pick the {recorded} variant, or\n"
                f"  - name this board explicitly with --port (you meant this one), or\n"
                f"  - pass --force if the map is out of date (then --learn the new variant)."
            )
            return 2
        print(
            f"\nnote: {board.get('mac')} is recorded as {recorded}, "
            f"you are flashing {variant} onto it (allowed by --port/--force)."
        )

    if args.dry_run:
        print("\n--dry-run: nothing written.")
        return 0

    if not args.yes:
        answer = input(f"\nFlash {variant} onto {board.get('mac')} ({board['dev']})? [y/N] ")
        if answer.strip().lower() not in ("y", "yes"):
            print("aborted.")
            return 2

    if idf_flash(variant, board["dev"]) != 0:
        print("\nflash failed: see the idf.py output above.", file=sys.stderr)
        return 1

    if args.learn and board.get("mac"):
        boards[board["mac"]] = variant
        save_map(boards)
        print(f"recorded {board['mac']} -> {variant} in {BOARD_MAP.relative_to(REPO)}")

    log_path = REPO / f"build_r_{variant}" / "boot_flash.log"
    log = capture_boot(board["dev"], log_path, args.capture)
    print(f"\nboot log: {log_path.relative_to(REPO)} ({len(log)} bytes)")
    missing, failures = check_boot(variant, log)
    for anchor in ANCHORS[variant]:
        print(f"  [{'ok' if anchor in log else 'missing'}] {anchor}")
    if failures:
        print(f"  failure patterns in the log: {', '.join(failures)}")
    return 0 if not missing and not failures else 1


if __name__ == "__main__":
    sys.exit(main())
