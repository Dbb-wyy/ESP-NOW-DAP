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

`tools/boards.json` is the learned map. Add a board by flashing it once with
`--learn`, or by editing the file (MAC in lower case, variant in
`wired|host|slave_s3|slave_c6|slave_c3`).

Exit codes: `0` done, `1` refused/failed, `2` needs a decision (no board, ambiguous
port, unknown board).

## `perf/` — the measurement harness

See [`perf/README.md`](perf/README.md) and `docs/perf.md` (the metrics and the
baseline).
