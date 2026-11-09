#!/usr/bin/env python3
"""Performance bench for the END_nightly wireless bridge (Plan_v3 T1.1).

One command per case, producing the metrics defined in `docs/perf.md` §1 (D1):

  rtt               one 64 B DAP round trip        -> microseconds
  bulk-read         dump_image <len> bytes         -> KiB/s (real bytes)
  bulk-write-ram    load_image <len> bytes to RAM  -> KiB/s (real bytes)
  flash-program     program <elf> verify reset     -> seconds end to end

Every case records a parameter snapshot (git HEAD, sdkconfig key items, board
ids, toolchain), the raw samples, n/min/p50/mean/p95/max, the classification of
each failure (hang vs DAP dead vs target error) and, on request, retries.

This tool is not part of the firmware build and does not touch `main/` or
`components/`.

Examples
--------
  tools/perf/bench.py --check
  tools/perf/bench.py rtt --swd-khz 2000 --reps 30
  tools/perf/bench.py bulk-read --swd-khz 2000 --len 32768 --reps 5
  tools/perf/bench.py bulk-write-ram --swd-khz 2000 --len 32768 --reps 5
  tools/perf/bench.py flash-program --target f4x \
      --elf test/shaobing_gimbalchangeauthor.elf --reps 10
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime

REPO = pathlib.Path(__file__).resolve().parents[2]
OPENOCD = shutil.which("openocd") or "/usr/bin/openocd"
TARGET_CFG = {"f1x": "target/stm32f1x.cfg", "f4x": "target/stm32f4x.cfg"}
DEFAULT_ADDR = {"f1x": "0x20001000", "f4x": "0x20000000"}
DEFAULT_LEN = {"f1x": 8192, "f4x": 32768}
DEFAULT_READ_ADDR = {"f1x": "0x08000000", "f4x": "0x08000000"}

HANG_KINDS = {"hang_during_program", "hang_before_target", "dap_dead"}

# Relative gate rules, applied against perf_out/baseline.json (--save-baseline).
# Absolute floors come from docs/perf.md §4 (Plan_v3 T4.1); they are written
# here as data so the tool and the doc cannot drift apart silently.
GATE_RULES = {
    "rtt": {"metric": "p99", "op": "<=", "factor": 0.8,
            "abs": 2100.0},
    "bulk-read": {"metric": "p50", "op": ">=", "factor": 1.3,
                  "abs": None},
    "bulk-write-ram": {"metric": "p50", "op": ">=", "factor": 1.3,
                       "abs": None},
    "flash-program": {"metric": "p50", "op": "<=", "factor": None,
                      "abs": 4.5},
}
BASELINE = "perf_out/baseline.json"

RE_NUM = re.compile(r"[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?")
RE_DUMP = re.compile(r"dumped (\d+) bytes in ([\d.]+)s \(([\d.]+) KiB/s\)")
RE_LOAD = re.compile(r"downloaded (\d+) bytes in ([\d.]+)s \(([\d.]+) KiB/s\)")
RE_RTT = re.compile(r"RTT_US=(\d+)")
RE_WROTE = re.compile(r"wrote (\d+) bytes from file .* in ([\d.]+)s \(([\d.]+) KiB/s\)")
RE_VERIFIED = re.compile(r"verified (\d+) bytes in ([\d.]+)s \(([\d.]+) KiB/s\)")


# --------------------------------------------------------------------------- #
# process helpers
# --------------------------------------------------------------------------- #

def run_oc(arglist: list[str], timeout: float, dry_run: bool = False) -> dict:
    """Run one openocd invocation; always returns a dict, never raises."""
    cmd = [OPENOCD] + arglist
    if dry_run:
        return {"cmd": cmd, "rc": None, "out": "", "wall_s": 0.0,
                "timed_out": False, "dry_run": True}

    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    timed_out = False
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
    wall = time.monotonic() - t0
    return {"cmd": cmd, "rc": proc.returncode, "out": out or "",
            "wall_s": round(wall, 3), "timed_out": timed_out,
            "dry_run": False}


def classify(run: dict) -> str:
    """Turn one openocd run into a status word (ok / hang / error kinds)."""
    out = run["out"]
    if run["timed_out"]:
        if "Programming Started" in out and "Programming Finished" not in out:
            return "hang_during_program"
        return "hang_before_target"
    if "CMSIS-DAP command CMD_INFO failed" in out:
        return "dap_dead"
    if "unable to find a matching CMSIS-DAP device" in out \
            or "could not open device" in out:
        return "dap_dead"
    if "Pipe error" in out:
        return "pipe_error"
    if "failed erasing sectors" in out:
        return "target_error_erase"
    if "error executing stm32x flash" in out or "flash write failed" in out:
        return "target_error_write"
    if "timeout waiting for algorithm" in out:
        return "algorithm_timeout"
    if "Error: Error setting register" in out:
        return "swd_margin_error"
    if run["rc"] != 0:
        return "error"
    return "ok"


def openocd_args(args) -> list[str]:
    """Interface/target selection plus the knobs that must precede `init`."""
    out = ["-f", "interface/cmsis-dap.cfg", "-f", TARGET_CFG[args.target]]
    if getattr(args, "vid_pid", None):
        out += ["-c", f"cmsis_dap_vid_pid {args.vid_pid}"]
    out += ["-c", f"adapter speed {args.swd_khz}"]
    return out


def with_script(args, script: list[str]) -> list[str]:
    out = openocd_args(args)
    for line in script:
        out += ["-c", line]
    return out


# --------------------------------------------------------------------------- #
# snapshot / helpers
# --------------------------------------------------------------------------- #

def sh(cmd: list[str], timeout: float = 10) -> str:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (p.stdout + p.stderr).strip()
    except Exception as exc:  # noqa: BLE001 - snapshot must never fail the run
        return f"<{type(exc).__name__}: {exc}>"


def sdkconfig_keys() -> dict:
    """A few Kconfig values that change the measured numbers.

    Every `sdkconfig.r_*` that exists on disk is recorded, so an A/B run with
    a non-default slave variant (slave_s3, slave_c3, *_ch11, ...) cannot be
    silently mislabelled as the default host/slave_c6 pair.
    """
    keys = ["CONFIG_IDF_TARGET=", "CONFIG_BRIDGE_ROLE_", "CONFIG_WLINK_CHANNEL=",
            "CONFIG_WLINK_TX_RATE_", "CONFIG_WLINK_SWD_TIMEOUT_MS=",
            "CONFIG_WLINK_SWD_RETRIES=", "CONFIG_WLINK_SWD_VERBOSE_LOG=",
            "CONFIG_COMPILER_OPTIMIZATION_", "CONFIG_LOG_DEFAULT_LEVEL="]
    found = {}
    for path in sorted(REPO.glob("sdkconfig.r_*")):
        if not path.is_file():
            continue
        vals = []
        for line in path.read_text(errors="replace").splitlines():
            if line.startswith("#"):
                continue
            if any(line.startswith(k) for k in keys):
                vals.append(line.strip())
        found[path.name.replace("sdkconfig.r_", "")] = sorted(vals)
    return found


def boards() -> dict:
    usb = [l.strip() for l in sh(["lsusb"]).splitlines()
           if "303a:1002" in l or "1a86:55d3" in l]
    by_id = {}
    by_id_dir = pathlib.Path("/dev/serial/by-id")
    if by_id_dir.is_dir():
        for link in sorted(by_id_dir.iterdir()):
            by_id[link.name] = os.path.realpath(link)
    return {"usb_matches": usb, "serial_by_id": by_id}


BY_ID_DIR = pathlib.Path("/dev/serial/by-id")


def resolve_serial(spec: str) -> str:
    """Map a console spec to a stable /dev/serial/by-id path.

    The ttyACM numbers change with every enumeration, so accept any of: a full
    path, the by-id link name, a fragment of that name, or a tty name (looked up
    through the by-id links) and hand back the symlink whenever one matches.
    """
    if not spec:
        return spec

    links = {}
    if BY_ID_DIR.is_dir():
        for link in sorted(BY_ID_DIR.iterdir()):
            links[link.name] = os.path.realpath(link)

    # Prefer a stable by-id link even when the caller passed /dev/ttyACM0:
    # tty numbers change on every enumeration, so the symlink is the provenance
    # we actually want in the JSON.
    if spec in links:
        return str(BY_ID_DIR / spec)
    for name in links:
        if spec in name:
            return str(BY_ID_DIR / name)

    if spec.startswith("tty"):
        wanted = os.path.realpath(f"/dev/{spec}")
        for name, real in links.items():
            if real == wanted:
                return str(BY_ID_DIR / name)

    return spec


def sha256_of(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot(args) -> dict:
    elf = pathlib.Path(args.elf) if args.elf else None
    if elf and not elf.is_absolute():
        elf = REPO / elf
    info = {
        "when": datetime.now().astimezone().isoformat(timespec="seconds"),
        "repo": str(REPO),
        "git_head": sh(["git", "-C", str(REPO), "rev-parse", "HEAD"]),
        "git_describe": sh(["git", "-C", str(REPO), "describe", "--tags", "--always"]),
        "git_dirty": sh(["git", "-C", str(REPO), "status", "--porcelain",
                         "--untracked-files=no"]),
        "openocd": sh([OPENOCD, "--version"]).splitlines()[:1],
        "python": f"{sys.version.split()[0]} ({sys.executable})",
        "uname": sh(["uname", "-smr"]),
        "target_cfg": TARGET_CFG[args.target],
        "swd_khz": args.swd_khz,
        "boards": boards(),
        "sdkconfig": sdkconfig_keys(),
        "elf": None,
    }
    if elf and elf.exists():
        info["elf"] = {"path": str(elf.relative_to(REPO)) if elf.is_relative_to(REPO) else str(elf),
                       "bytes": elf.stat().st_size, "sha256": sha256_of(elf)}
    return info


def percentile(ordered: list[float], q: float) -> float:
    """Nearest-rank-ish percentile; q in [0, 1]. ordered must be sorted."""
    if not ordered:
        raise ValueError("empty sample list")
    idx = int(round(q * (len(ordered) - 1)))
    return ordered[max(0, min(len(ordered) - 1, idx))]


def stats(values: list[float]) -> dict:
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    return {
        "n": len(values),
        "min": round(ordered[0], 3),
        "p50": round(statistics.median(ordered), 3),
        "mean": round(statistics.fmean(ordered), 3),
        "p95": round(percentile(ordered, 0.95), 3),
        "p99": round(percentile(ordered, 0.99), 3),
        "max": round(ordered[-1], 3),
    }


def pulse_rts(port: str, seconds: float = 0.02) -> str:
    """Reset the host board through its console CH340 (no DTR/RTS default pull)."""
    try:
        import serial  # pyserial, provided by the conda env
    except ImportError:
        return "no pyserial"
    try:
        with serial.Serial(port, 115200, timeout=1) as ser:
            ser.dtr = False
            ser.rts = True
            time.sleep(seconds)
            ser.rts = False
        return f"rts pulse on {port}"
    except Exception as exc:  # noqa: BLE001
        return f"rts pulse failed: {type(exc).__name__}: {exc}"


# --------------------------------------------------------------------------- #
# cases
# --------------------------------------------------------------------------- #

def case_rtt(args) -> dict:
    script = ["init", "halt"]
    script.append(f'puts "TARGET_OK"')
    script.append(f'for {{set i 0}} {{$i < {args.reps}}} {{incr i}} {{'
                  f' puts "RTT_US=[lindex [time {{mdw {args.addr} 1}} 1] 0]" }}')
    script += ["shutdown"]
    return _run_single_session(args, "rtt", script, [RE_RTT])


def case_bulk_read(args) -> dict:
    tmp = pathlib.Path("/tmp") / f"bench_dump_{os.getpid()}.bin"
    script = ["init", "halt", 'puts "TARGET_OK"']
    for _ in range(args.reps):
        script.append(f'dump_image {tmp} {args.addr} {args.len}')
    script.append("shutdown")
    result = _run_single_session(args, "bulk-read", script, [RE_DUMP])
    tmp.unlink(missing_ok=True)
    return result


def case_bulk_write_ram(args) -> dict:
    payload = pathlib.Path("/tmp") / f"bench_payload_{os.getpid()}.bin"
    payload.write_bytes(bytes((i * 7 + 13) & 0xFF for i in range(args.len)))
    script = ["init", "halt", 'puts "TARGET_OK"']
    for _ in range(args.reps):
        script.append(f'load_image {payload} {args.addr} bin')
    script.append("shutdown")
    result = _run_single_session(args, "bulk-write-ram", script, [RE_LOAD])
    payload.unlink(missing_ok=True)
    return result


def case_flash_program(args) -> dict:
    elf = pathlib.Path(args.elf)
    if not elf.is_absolute():
        elf = REPO / elf
    runs, samples, failures = [], [], []
    attempts = 0
    for rep in range(args.reps):
        attempt = 0
        while True:
            attempt += 1
            attempts += 1
            script = ["init", f"program {elf} verify reset", "shutdown"]
            run = run_oc(with_script(args, script), args.timeout, args.dry_run)
            kind = "dry_run" if run.get("dry_run") else classify(run)
            ok = kind == "ok" and "Programming Finished" in run["out"] \
                and "Verified OK" in run["out"]
            entry = {"rep": rep + 1, "attempt": attempt, "status": kind,
                     "wall_s": run["wall_s"], "rc": run["rc"]}
            if not ok:
                entry["detail"] = run["out"][-600:]
            m = RE_WROTE.search(run["out"])
            if m:
                entry["write_kibs"] = float(m.group(3))
                entry["write_s"] = float(m.group(2))
            m = RE_VERIFIED.search(run["out"])
            if m:
                entry["verify_kibs"] = float(m.group(3))
                entry["verify_s"] = float(m.group(2))
            runs.append(entry)
            print(f"[flash {rep + 1}/{args.reps}] attempt {attempt}: {kind} "
                  f"wall={run['wall_s']}s", file=sys.stderr, flush=True)
            if ok:
                samples.append(run["wall_s"])
                break
            failures.append(entry)
            if kind in HANG_KINDS and args.recover:
                target = args.host_console
                if target:
                    entry["recover"] = pulse_rts(target)
                    time.sleep(3)
            if attempt > args.retries:
                break
    return {"case": "flash-program", "samples": samples, "runs": runs,
            "failures": failures, "attempts": attempts,
            "ok": len(samples), "requested": args.reps}


def _run_single_session(args, case: str, script: list[str], patterns: list[re.Pattern]) -> dict:
    """Run one openocd session whose script prints the samples; parse them.

    Every attempt is kept in `runs`, and every failed attempt in `failures` --
    a retry that eventually succeeds must not erase the fact that the link
    failed once. `ok` is the number of samples, `requested` the number asked
    for, so `ok != requested` is the honest success signal.
    """
    attempts = 0
    samples: list[float] = []
    runs: list[dict] = []
    failures: list[dict] = []
    last_run: dict = {}
    while True:
        attempts += 1
        run = run_oc(with_script(args, script), args.timeout, args.dry_run)
        last_run = run
        if run.get("dry_run"):
            return {"case": case, "samples": [], "runs": [{"attempt": 1,
                    "status": "dry_run", "cmd": run["cmd"]}],
                    "failures": [], "attempts": 1, "ok": 0,
                    "requested": args.reps}
        kind = classify(run)
        print(f"[{case}] attempt {attempts}: {kind} wall={run['wall_s']}s",
              file=sys.stderr, flush=True)
        if "TARGET_OK" not in run["out"]:
            entry = {"attempt": attempts, "status": kind,
                     "wall_s": run["wall_s"], "detail": run["out"][-600:]}
            failures.append(entry)
            runs.append(entry)
            if args.recover and args.host_console and kind in HANG_KINDS:
                entry["recover"] = pulse_rts(args.host_console)
                time.sleep(3)
            if attempts > args.retries:
                break
            continue
        samples = _parse_samples(run["out"], case, patterns)
        runs.append({"attempt": attempts,
                     "status": "ok" if samples else "empty",
                     "wall_s": run["wall_s"], "samples": len(samples)})
        if samples or attempts > args.retries:
            break
    result = {"case": case, "samples": samples, "attempts": attempts,
              "ok": len(samples), "requested": args.reps, "runs": runs,
              "failures": failures}
    if not samples and not args.dry_run:
        result["last_output_tail"] = last_run.get("out", "")[-600:]
    return result


def _parse_samples(out: str, case: str, patterns: list[re.Pattern]) -> list[float]:
    if case == "rtt":
        return [float(v) for v in RE_RTT.findall(out)]
    samples = []
    for pat in patterns:
        for m in pat.finditer(out):
            samples.append(float(m.group(3)))  # KiB/s is always the last group
    return samples


# --------------------------------------------------------------------------- #
# output
# --------------------------------------------------------------------------- #

def summarise(result: dict) -> dict:
    unit = {"rtt": "us", "bulk-read": "KiB/s", "bulk-write-ram": "KiB/s",
            "flash-program": "s"}[result["case"]]
    result["unit"] = unit
    st = stats(result["samples"])
    result["stats"] = st
    return result


def write_outputs(result: dict, snapshot_data: dict, args, started: float) -> dict:
    payload = {
        "case": result["case"],
        "started": snapshot_data["when"],
        "duration_s": round(time.monotonic() - started, 2),
        "params": {k: v for k, v in vars(args).items()},
        "snapshot": snapshot_data,
        "unit": result["unit"],
        "stats": result["stats"],
        "samples": result["samples"],
        "runs": result.get("runs", []),
        "failures": result.get("failures", []),
        "attempts": result.get("attempts"),
        "ok": result.get("ok"),
        "requested": result.get("requested"),
    }
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = REPO / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    base = out_dir / f"{stamp}-{result['case']}"
    json_path, md_path = base.with_suffix(".json"), base.with_suffix(".md")
    json_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n")
    md_path.write_text(render_markdown(payload), encoding="utf-8")
    print(f"\nJSON: {json_path.relative_to(REPO)}\nMD  : {md_path.relative_to(REPO)}")
    return payload


def render_markdown(payload: dict) -> str:
    st, unit = payload["stats"], payload["unit"]
    s = payload["snapshot"]
    lines = [f"# bench · {payload['case']}", "",
             f"- when: `{payload['started']}`",
             f"- duration: {payload['duration_s']} s",
             f"- git: `{s.get('git_head', '?')[:12]}`"
             + (" (dirty)" if s.get("git_dirty") else ""),
             f"- openocd: `{s.get('openocd', ['?'])[0]}`",
             f"- target: `{s.get('target_cfg')}` @ {s.get('swd_khz')} kHz",
             f"- params: `{json.dumps(payload['params'], ensure_ascii=False)}`", ""]
    if st.get("n"):
        lines += ["| n | min | p50 | mean | p95 | p99 | max | unit |",
                  "|---|---|---|---|---|---|---|---|",
                  f"| {st['n']} | {st['min']} | {st['p50']} | {st['mean']} | "
                  f"{st['p95']} | {st.get('p99')} | {st['max']} | {unit} |", ""]
    else:
        lines += ["_no samples_", ""]
    if payload["case"] == "flash-program":
        lines += [f"successful runs: **{payload.get('ok')} / {payload.get('requested')}**"
                  f" ({payload.get('attempts')} attempts)", ""]
        if payload.get("runs"):
            lines += ["| rep | attempt | status | wall s |", "|---|---|---|---|"]
            for r in payload["runs"]:
                lines.append(f"| {r['rep']} | {r['attempt']} | `{r['status']}` | {r['wall_s']} |")
            lines.append("")
    fails = payload.get("failures") or []
    if fails:
        kinds = {}
        for f in fails:
            kinds[f.get("status", "?")] = kinds.get(f.get("status", "?"), 0) + 1
        lines += [f"failures: {kinds}", ""]
    elf = s.get("elf")
    if elf:
        lines += [f"- elf: `{elf['path']}` {elf['bytes']} B sha256 `{elf['sha256'][:16]}…`", ""]
    sdk = s.get("sdkconfig") or {}
    for variant, vals in sdk.items():
        lines.append(f"- `sdkconfig.r_{variant}`: " + ", ".join(f"`{v}`" for v in vals))
    return "\n".join(lines) + "\n"


def do_check(args) -> int:
    snap = snapshot(args)
    print("== environment ==")
    for key in ("git_head", "git_describe", "openocd", "python", "uname", "target_cfg", "swd_khz"):
        print(f"{key:12s} {snap[key]}")
    print("== boards ==")
    usb = snap["boards"]["usb_matches"]
    dap = [line for line in usb if "303a:1002" in line]
    console = [line for line in usb if "1a86:55d3" in line]
    for line in usb:
        print(" ", line)
    for name, real in snap["boards"]["serial_by_id"].items():
        print(f"  {name} -> {real}")
    if not dap:
        print("== verdict == no CMSIS-DAP (303a:1002) attached")
        return 1
    if not console:
        print("== verdict == DAP ready, but no host console CH340 (1a86:55d3) -- "
              "--recover will not work")
        return 0
    if not pathlib.Path(OPENOCD).exists():
        print("== verdict == MISSING openocd")
        return 1
    print("== verdict == ready")
    return 0


# --------------------------------------------------------------------------- #
# gate / baseline
# --------------------------------------------------------------------------- #

def load_baseline() -> dict:
    path = REPO / BASELINE
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:  # noqa: BLE001
        print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
        return {}


def save_baseline(payload: dict) -> None:
    path = REPO / BASELINE
    data = load_baseline()
    data[payload["case"]] = {
        "when": payload["started"],
        "stats": payload["stats"],
        "unit": payload["unit"],
        "swd_khz": payload["snapshot"].get("swd_khz"),
        "target_cfg": payload["snapshot"].get("target_cfg"),
        "git_head": payload["snapshot"].get("git_head"),
        "params": payload.get("params", {}),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"baseline updated: {path.relative_to(REPO)}")


def gate_report(payload: dict, baseline: dict) -> list[tuple[str, bool, str]]:
    """Compare one case against GATE_RULES + perf_out/baseline.json."""
    case, st = payload["case"], payload["stats"]
    if not st.get("n"):
        return [("samples", False, "no samples")]
    rule = GATE_RULES.get(case)
    if not rule:
        return [("samples", True, "no gate rule for this case")]
    metric = rule["metric"]
    cur = st.get(metric)
    base = (baseline.get(case) or {}).get("stats") or {}
    limit = rule.get("abs")
    note = "absolute threshold"
    if rule.get("factor") and base.get(metric):
        rel = base[metric] * rule["factor"]
        if limit is None:
            limit, note = rel, f"{rule['factor']}x baseline {base[metric]}"
        elif rule["op"] == "<=" and rel < limit:
            limit, note = rel, f"{rule['factor']}x baseline {base[metric]}"
    if limit is None:
        return [(f"{metric}", True,
                 f"{metric}={cur} recorded (no baseline/threshold yet)")]
    ok = cur <= limit if rule["op"] == "<=" else cur >= limit
    return [(f"{metric} {rule['op']} {round(limit, 3)}", ok,
             f"{metric}={cur} ({note})")]


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def main(argv: list[str]) -> int:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--swd-khz", type=int, default=2000,
                        help="SWD clock in kHz (default 2000)")
    common.add_argument("--target", choices=sorted(TARGET_CFG), default="f4x")
    common.add_argument("--reps", type=int, default=None)
    common.add_argument("--timeout", type=float, default=120.0,
                        help="per openocd invocation timeout (s)")
    common.add_argument("--retries", type=int, default=1,
                        help="extra attempts after a hang/error (default 1)")
    common.add_argument("--recover", action="store_true",
                        help="pulse RTS on the host console after a hang/dap_dead")
    common.add_argument("--host-console-serial", default="54E2053871",
                        help="USB serial of the host board console (looked up by-id)")
    common.add_argument("--host-console", default=None,
                        help="host console tty (default: auto from by-id)")
    common.add_argument("--vid-pid", default="0x303a 0x1002")
    common.add_argument("--dry-run", action="store_true")
    common.add_argument("--out-dir", default="perf_out")
    common.add_argument("--gate", action="store_true",
                        help="compare this run against perf_out/baseline.json "
                             "and the absolute thresholds in GATE_RULES")
    common.add_argument("--save-baseline", action="store_true",
                        help="record this run's stats into perf_out/baseline.json")

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="case", required=False)
    ap.add_argument("--check", action="store_true", help="print environment only")
    for name in ("rtt", "bulk-read", "bulk-write-ram", "flash-program"):
        p = sub.add_parser(name, parents=[common])
        p.add_argument("--addr", default=None)
        p.add_argument("--len", type=int, default=None)
        p.add_argument("--elf", default=None,
                       help="ELF to flash (required by flash-program)")
    args = ap.parse_args(argv)

    if getattr(args, "check", False):
        return do_check(argparse.Namespace(elf=None, target="f4x", swd_khz=2000))
    if args.case is None:
        ap.print_help()
        return 1
    if args.case == "flash-program" and not args.elf:
        ap.error("flash-program requires an explicit --elf <image.elf> "
                 "(the image sha256 is part of the provenance)")

    # defaults that depend on target / case
    defaults = {"rtt": 30, "bulk-read": 5, "bulk-write-ram": 5, "flash-program": 10}
    if args.reps is None:
        args.reps = defaults[args.case]
    if args.addr is None:
        args.addr = DEFAULT_READ_ADDR[args.target] if args.case == "rtt" \
            else DEFAULT_ADDR[args.target] if args.case == "bulk-write-ram" \
            else DEFAULT_READ_ADDR[args.target]
    if args.len is None:
        args.len = DEFAULT_LEN[args.target]
    args.host_console = resolve_serial(args.host_console) if args.host_console else None
    if not args.host_console:
        for name, real in boards()["serial_by_id"].items():
            if args.host_console_serial in name:
                args.host_console = real

    if args.dry_run:
        sample = {
            "rtt": ["init", "halt", 'puts "TARGET_OK"',
                    'puts "RTT_US=[lindex [time {mdw %s 1} 1] 0]"' % args.addr, "shutdown"],
            "bulk-read": ["init", "halt", "puts \"TARGET_OK\"",
                          f"dump_image /tmp/x.bin {args.addr} {args.len}", "shutdown"],
            "bulk-write-ram": ["init", "halt", "puts \"TARGET_OK\"",
                               f"load_image /tmp/payload.bin {args.addr} bin", "shutdown"],
            "flash-program": ["init", f"program {args.elf} verify reset", "shutdown"],
        }[args.case]
        print("openocd " + " ".join(shlex.quote(x) for x in with_script(args, sample)))
        print(f"# reps={args.reps} timeout={args.timeout}s retries={args.retries}")
        return 0

    started = time.monotonic()
    snap = snapshot(args)
    runner = {"rtt": case_rtt, "bulk-read": case_bulk_read,
              "bulk-write-ram": case_bulk_write_ram,
              "flash-program": case_flash_program}[args.case]
    result = summarise(runner(args))
    print(f"case={result['case']} unit={result['unit']} "
          f"n={result['stats'].get('n', 0)} attempts={result.get('attempts')}")
    if result["stats"].get("n"):
        st = result["stats"]
        print(f"  min={st['min']} p50={st['p50']} mean={st['mean']} "
              f"p95={st['p95']} p99={st.get('p99')} max={st['max']} "
              f"{result['unit']}")
    if result.get("failures"):
        print(f"  failures: {[f.get('status') for f in result['failures']]}")
    payload = write_outputs(result, snap, args, started)
    if getattr(args, "save_baseline", False):
        save_baseline(payload)
    gate_failed = False
    if getattr(args, "gate", False):
        checks = gate_report(payload, load_baseline())
        print("\n== gate ==")
        for name, passed, detail in checks:
            print(f"  [{'PASS' if passed else 'FAIL'}] {name}: {detail}")
        gate_failed = any(not passed for _, passed, _ in checks)
    ok = bool(result["stats"].get("n"))
    if gate_failed:
        return 2
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
