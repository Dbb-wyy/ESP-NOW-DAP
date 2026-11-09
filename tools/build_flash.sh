#!/usr/bin/env bash
# Build and flash the ESP-NOW-DAP boards in one step.
#
#   tools/build_flash.sh                 # build + flash host and slave_c6
#   tools/build_flash.sh host slave_c6   # explicit variants, serial order
#   tools/build_flash.sh --no-flash      # build only
#   tools/build_flash.sh --no-build      # flash the existing build_r_<v> images
#   tools/build_flash.sh --dry-run       # print the plan, touch nothing
#
# Why this exists: the 24 Mbps wireless build needs the per-role defaults chain
# (sdkconfig.defaults + sdkconfig.defaults.wireless_*); omitting it silently
# builds a 1 Mbps firmware. Flashing must go through the by-id ports and the
# MAC -> variant map in tools/boards.json, never through ttyACM<N>. This script
# binds those steps together, verifies the build assertions and the boot-log
# anchors, and stops on the first failure instead of flashing a half-built tree.
set -uo pipefail

# The ESP-IDF activation script refuses to be sourced from a script whose "$0"
# is a file name (its is_sourced() check looks for dash/bash/sh). Re-exec
# ourselves under `bash -c` once so that "$0" is exactly "bash" inside, then
# the source below is accepted. The re-exec is transparent: same args, same cwd.
if [ "${0##*/}" != "bash" ] && [ -z "${BFL_REEXEC:-}" ]; then
    BFL_REEXEC=1 exec bash -c 'f=$1; shift; source "$f" "$@"' bash "$0" "$@"
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_ENV="${IDF_ENV:-$HOME/.espressif/tools/activate_idf_v6.0.2.sh}"
CAPTURE_SECONDS=8
DO_BUILD=1
DO_FLASH=1
ASSUME_YES=0
DRY_RUN=0
VARIANTS=()

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

while (($#)); do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-flash) DO_FLASH=0 ;;
        -y|--yes) ASSUME_YES=1 ;;
        --dry-run) DRY_RUN=1 ;;
        --capture) CAPTURE_SECONDS="${2:-8}"; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "unknown option: $1" >&2; usage; exit 2 ;;
        *) VARIANTS+=("$1") ;;
    esac
    shift
done
((${#VARIANTS[@]})) || VARIANTS=(host slave_c6)

chip_for() {
    case "$1" in
        wired|host|slave_s3) echo esp32s3 ;;
        slave_c6) echo esp32c6 ;;
        slave_c3) echo esp32c3 ;;
        *) return 1 ;;
    esac
}

defaults_for() {
    case "$1" in
        wired) echo sdkconfig.defaults.wired ;;
        host) echo sdkconfig.defaults.wireless_host ;;
        slave_*) echo sdkconfig.defaults.wireless_slave ;;
        *) return 1 ;;
    esac
}

role_for() {
    case "$1" in
        wired) echo CONFIG_BRIDGE_ROLE_WIRED=y ;;
        host) echo CONFIG_BRIDGE_ROLE_WIRELESS_HOST=y ;;
        slave_*) echo CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE=y ;;
        *) return 1 ;;
    esac
}

# Build one variant and verify the four build criteria. Sets BUILD_RESULT[$v].
declare -A BUILD_RESULT=()
build_variant() {
    local v="$1" chip defaults role log
    chip="$(chip_for "$v")" || die "unknown variant '$v' (wired|host|slave_s3|slave_c6|slave_c3)"
    defaults="$(defaults_for "$v")"
    role="$(role_for "$v")"
    log="$REPO_ROOT/build_r_$v/build_skill.log"

    echo
    echo "== build $v ($chip)"
    echo "   defaults: sdkconfig.defaults;$defaults"
    echo "   log:      ${log#$REPO_ROOT/}"

    if ((DRY_RUN)); then
        echo "   (dry-run) idf.py -B build_r_$v -D SDKCONFIG=sdkconfig.r_$v \\"
        echo "                     -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;$defaults\" build"
        BUILD_RESULT[$v]="dry-run"
        return 0
    fi

    idf.py -B "build_r_$v" -D "SDKCONFIG=sdkconfig.r_$v" \
        -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;$defaults" build 2>&1 | tee "$log"
    if ((${PIPESTATUS[0]} != 0)); then
        BUILD_RESULT[$v]="build: exit ${PIPESTATUS[0]}"
        return 1
    fi
    grep -q "Project build complete" "$log" || {
        BUILD_RESULT[$v]="build: no 'Project build complete'"
        return 1
    }

    local cfg="$REPO_ROOT/sdkconfig.r_$v"
    grep -q "CONFIG_IDF_TARGET=\"$chip\"" "$cfg" || {
        BUILD_RESULT[$v]="build: target is not $chip"; return 1; }
    grep -q "$role" "$cfg" || { BUILD_RESULT[$v]="build: role is not $role"; return 1; }
    if [ "$v" != wired ]; then
        grep -q "CONFIG_WLINK_TX_RATE_24M=y" "$cfg" || {
            BUILD_RESULT[$v]="build: TX rate is not 24M (silent 1 Mbps build?)"; return 1; }
        grep -q "CONFIG_WLINK_CHANNEL=1" "$cfg" || {
            BUILD_RESULT[$v]="build: channel is not 1"; return 1; }
    fi

    # A stale or missing image is a build failure, not a flash problem.
    [ -s "$REPO_ROOT/build_r_$v/bridge.bin" ] || {
        BUILD_RESULT[$v]="build: bridge.bin missing"; return 1; }

    idf.py -B "build_r_$v" size > "$REPO_ROOT/build_r_$v/size_skill.log" 2>&1 || {
        BUILD_RESULT[$v]="build: size failed"; return 1; }
    local image_size warnings
    image_size="$(grep -o 'Total image size: [0-9]* bytes' "$REPO_ROOT/build_r_$v/size_skill.log" | tail -1 | grep -o '[0-9]*')"
    warnings="$(grep -c 'warning:' "$log" 2>/dev/null || true)"
    echo "   OK: Total image size ${image_size:-?} bytes, warnings ${warnings:-0}"
    BUILD_RESULT[$v]="ok (${image_size:-?} B, w=${warnings:-0})"
    return 0
}

# Flash one variant through tools/flash.py (identity from by-id + MAC map),
# retrying once on transient probe/connect errors only.
declare -A FLASH_RESULT=()
flash_variant() {
    local v="$1" log
    log="$REPO_ROOT/build_r_$v/flash_skill.log"

    echo
    echo "== flash $v"
    if ((DRY_RUN)); then
        echo "   (dry-run) python3 tools/flash.py --variant $v --yes --capture $CAPTURE_SECONDS"
        FLASH_RESULT[$v]="dry-run"
        return 0
    fi

    local attempt
    for attempt in 1 2; do
        python3 "$REPO_ROOT/tools/flash.py" --variant "$v" --yes \
            --capture "$CAPTURE_SECONDS" 2>&1 | tee "$log"
        local rc=${PIPESTATUS[0]}
        ((rc == 0)) && {
            FLASH_RESULT[$v]="ok"
            grep -q "transmit rate" "$REPO_ROOT/build_r_$v/boot_flash.log" 2>/dev/null \
                && echo "   note: $(grep -o 'ESP-NOW transmit rate.*' "$REPO_ROOT/build_r_$v/boot_flash.log" | tail -1)"
            return 0
        }
        # Refusals and identity mismatches are real errors: do not retry them.
        if grep -Eqi "refus|mismatch|not mapped|stale|different role" "$log"; then
            FLASH_RESULT[$v]="refused (see above)"
            return 1
        fi
        if ((attempt == 1)); then
            echo "   transient flash failure; retrying once in 3 s ..."
            sleep 3
        fi
    done
    FLASH_RESULT[$v]="flash failed"
    return 1
}

echo "repo: $REPO_ROOT"
echo "idf:  $(grep -m1 'let idf_path' "$IDF_ENV" 2>/dev/null | sed 's/.*=//;s/[" ]//g')"

[ -f "$IDF_ENV" ] || die "ESP-IDF env script not found: $IDF_ENV (set IDF_ENV=...)"
# The IDF activation script is not written for `set -u`; relax it only while
# sourcing so one unset variable in IDF's own shell code cannot abort us.
set +u
# shellcheck disable=SC1090
source "$IDF_ENV" >/dev/null 2>&1
rc=$?
set -u
((rc == 0)) || die "failed to source $IDF_ENV"
command -v idf.py >/dev/null || die "idf.py not on PATH after sourcing $IDF_ENV"

if ((DO_FLASH)) && (( ! DRY_RUN )); then
    ls /dev/serial/by-id/* >/dev/null 2>&1 || \
        echo "WARN: /dev/serial/by-id is empty - flash will fail until a board is plugged in"
fi

failed=0
if ((DO_BUILD)); then
    for v in "${VARIANTS[@]}"; do
        build_variant "$v" || { failed=1; echo "   FAILED: ${BUILD_RESULT[$v]}"; break; }
    done
fi

if ((failed == 0)) && ((DO_FLASH)); then
    for v in "${VARIANTS[@]}"; do
        flash_variant "$v" || { failed=1; break; }
    done
fi

echo
echo "================ summary ================"
printf '%-10s %-34s %s\n' "variant" "build" "flash"
for v in "${VARIANTS[@]}"; do
    printf '%-10s %-34s %s\n' "$v" "${BUILD_RESULT[$v]:-skipped}" "${FLASH_RESULT[$v]:-skipped}"
done
if ((failed)); then
    echo "RESULT: FAILED"
    exit 1
fi
echo "RESULT: OK"
