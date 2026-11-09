# tools/

Helper scripts for bringing a board up. None of them is part of the firmware
build; `git` tracks them so a fresh checkout can use them.

## `flash.py` — find the board, then flash the variant it runs

The bridge has five variants and the two wireless boards are the same chip, so
`/dev/ttyACM0` says nothing about what is plugged in: the port numbers change on
every re-enumeration. `flash.py` derives the identity from the board itself and
only then writes:

1. **enumerate** `/dev/serial/by-id` (stable names) — `--list` stops here and does
   not touch the boards;
2. **probe** the chip and MAC with `esptool flash-id` (read-only query of the ROM
   loader; the board resets into the loader for a moment);
3. **resolve the variant** from chip + MAC via `tools/boards.json`, and cross-check
   it against the role and target compiled into `build_r_<variant>`;
4. **refuse** if the image is stale (older than the sources), if the build reports a
   different role, or if two boards of the right chip are plugged in and neither is
   mapped;
5. **print the table and ask** — nothing is flashed without a `y`;
6. flash through `idf.py -B build_r_<variant> -p <port> flash`, then capture the boot
   log and check the role anchors (and the panic/brownout patterns).

```bash
tools/flash.py --list                    # ports only, no probing
tools/flash.py --probe                   # chip + MAC of everything plugged in
tools/flash.py --variant host            # print the plan, ask, flash
tools/flash.py --variant slave_s3 --yes  # unattended (CI, repeated tests)
tools/flash.py --variant slave_s3 --learn  # also record MAC -> variant
```

## Flash both boards with one command

With both boards plugged in, `--all` probes them, resolves each variant from the map
and flashes them **in parallel** (each board is its own USB device, so the two
`idf.py` runs do not share anything: different `build_r_<variant>` directory,
different `sdkconfig.r_<variant>`):

```bash
tools/flash.py --all --dry-run     # what would be flashed, per board
tools/flash.py --all               # both boards, one confirmation
tools/flash.py --all --yes         # unattended
tools/flash.py --pair              # same, but refuse unless one host + one slave is plugged
tools/flash.py --all --jobs 1      # serialise the flashes (slowest, safest on a hub)
```

Each worker flashes, captures its own boot log (`build_r_<variant>/boot_flash.log`)
and checks that variant's anchors, and the summary at the end is per board, so a
failure on one board still reports the other. Boards that are not in the map are
skipped with a warning; two boards mapped to the same variant is a hard error.

Note: probing resets each board into the ROM loader, so expect the link to re-pair
about a second after the flash (the 1 s beacon does it on its own).

`tools/boards.json` is the learned map. Add a board by flashing it once with
`--learn`, or by editing the file (MAC in lower case, variant in
`wired|host|slave_s3|slave_c6|slave_c3`).

Exit codes: `0` done, `1` refused/failed, `2` needs a decision (no board, ambiguous
port, unknown board, `--pair` without a host or a slave).

## `perf/` — the measurement harness

See [`perf/README.md`](perf/README.md) and `docs/perf.md` (the metrics and the
baseline).

## `build_flash.sh` — build and flash the two wireless boards in one step

`flash.py` flashes an image; `build_flash.sh` produces the right image first and
binds the two steps together, because the 24 Mbps build depends on the per-role
defaults chain and a build without it silently produces a **1 Mbps** firmware.

```bash
tools/build_flash.sh                 # build + flash host and slave_c6 (default)
tools/build_flash.sh host slave_c6   # explicit variants, flashed in this order
tools/build_flash.sh --no-flash      # build only (e.g. before a soak test)
tools/build_flash.sh --no-build      # flash the existing build_r_<v> images
tools/build_flash.sh --dry-run       # print the plan, touch nothing
tools/build_flash.sh -y              # no prompts (flash.py --yes)
```

Per variant it runs `idf.py -B build_r_<v> -D SDKCONFIG=sdkconfig.r_<v>
-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<role>" build`,
then asserts exit 0, `Project build complete`, target/role/24M/channel in
`sdkconfig.r_<v>`, and that `bridge.bin` exists; it then calls
`tools/flash.py --variant <v> --yes`, which derives the board identity from
`/dev/serial/by-id` + `esptool flash-id` + `tools/boards.json`, refuses on a
stale image or a role mismatch, flashes, captures the boot log and checks the
role anchors (including `ESP-NOW transmit rate ... 24 Mbps` when the peer is up).
It stops at the first failure, retries once on transient probe/connect errors,
and prints a per-variant build/flash summary with `RESULT: OK|FAILED`.

Two things the script hides on purpose:

* **the IDF activation script** — it only accepts being sourced from a shell
  whose `$0` looks like `bash`/`sh`, so a script that sources it directly aborts;
  `build_flash.sh` re-execs itself under `bash -c` once so `$0` is `bash`;
* **the serial syntax** — `idf.py -D SDKCONFIG=...` must be set per variant, and
  flashing must never name a `ttyACM<N>` port.
