#!/usr/bin/env zsh
# Writes a file to the external SPI flash (MX25L1606E, 2 MiB) on the Rewair
# board via OpenOCD + the WICED SDK's waf_sflash_write RAM stub, then
# verifies the write by sampling readback over the device's dev-gated
# /api/debug/sflash HTTP route.
#
# Usage:
#   scripts/flash_sflash_openocd.zsh <image-file> <hexaddr>
#
# Env:
#   REWAIR_IP    device IP for readback verification (default 192.168.1.242)
#   FORCE=1      allow addr < 0x1C0000 (the UI region we own); refused
#                otherwise, since lower addresses contain OTA staging,
#                rollback, journal, and other reserved content.
#   OPENOCD      OpenOCD executable (auto-detected, including an unlinked
#                Homebrew open-ocd formula)
#   SDK_DIR      path to the WICED SDK checkout (default ../third_party/wiced-emw3165)
#   CHIP         probe-rs chip name for the pre/post reset (default STM32F411CE)
#
# Requires: a CMSIS-DAP probe (e.g. picoprobe) over SWD, brew-installed
# openocd (0.12+; the SDK's vendored 2015 OpenOCD binaries do not speak
# CMSIS-DAP), and probe-rs (used only to reset the target back into the
# normal application after the OpenOCD session -- OpenOCD and probe-rs
# cannot share the probe at the same time, so any probe-rs session must
# already be stopped before running this script).
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
REWAIR_IP="${REWAIR_IP:-192.168.1.242}"
FORCE="${FORCE:-0}"
OPENOCD="${OPENOCD:-}"

if [[ -z "$OPENOCD" ]]; then
    if command -v openocd > /dev/null 2>&1; then
        OPENOCD="$(command -v openocd)"
    elif [[ -x /opt/homebrew/opt/open-ocd/bin/openocd ]]; then
        OPENOCD="/opt/homebrew/opt/open-ocd/bin/openocd"
    elif [[ -x /usr/local/opt/open-ocd/bin/openocd ]]; then
        OPENOCD="/usr/local/opt/open-ocd/bin/openocd"
    fi
fi

if [[ -z "$OPENOCD" || ! -x "$OPENOCD" ]]; then
    print -u2 -r -- "OpenOCD not found; install open-ocd or set OPENOCD to its executable"
    exit 127
fi

UI_REGION_START=0x1C0000

if [[ $# -ne 2 ]]; then
    print -u2 -r -- "usage: $0 <image-file> <hexaddr>"
    exit 1
fi

image="$1"
addr_arg="$2"

if [[ ! -f "$image" ]]; then
    print -u2 -r -- "image file not found: $image"
    exit 1
fi

# Normalize hexaddr (accept with or without 0x/0X prefix) and validate it's hex.
addr_hex="${addr_arg#0x}"
addr_hex="${addr_hex#0X}"
if [[ ! "$addr_hex" =~ '^[0-9a-fA-F]+$' ]]; then
    print -u2 -r -- "bad hex address: $addr_arg"
    exit 1
fi
addr=$(( 0x$addr_hex ))

image_size=$(wc -c < "$image" | tr -d ' ')
device_capacity=$(( 0x200000 ))

if (( addr >= device_capacity || image_size > device_capacity - addr )); then
    print -u2 -r -- "refusing: [0x$(printf %x $addr), +0x$(printf %x $image_size)) does not fit in the 2 MiB device"
    exit 1
fi

if (( addr < UI_REGION_START )) && [[ "$FORCE" != "1" ]]; then
    print -u2 -r -- "refusing to write below the UI region (0x$(printf %x $UI_REGION_START)): 0x$(printf %x $addr) is outside it."
    print -u2 -r -- "0x000000-0x1BFFFF contains OTA staging, rollback, journal, and other reserved content."
    print -u2 -r -- "Set FORCE=1 to override."
    exit 1
fi

STUB_BUILD_NAME="waf_sflash_write-NoOS-NoNS-AWAIR"
STUB_ELF="$SDK_DIR/build/$STUB_BUILD_NAME/binary/$STUB_BUILD_NAME.elf"

if [[ ! -f "$STUB_ELF" ]]; then
    print -r -- "Building $STUB_BUILD_NAME (not found at $STUB_ELF)..."
    "$repo_root/scripts/sync_to_wiced.zsh"
    ( cd "$SDK_DIR" && tools/common/OSX/make "waf.sflash_write-NoOS-NoNS-AWAIR" HOST_OS=OSX )
fi

if [[ ! -f "$STUB_ELF" ]]; then
    print -u2 -r -- "build did not produce $STUB_ELF"
    exit 1
fi

print -r -- "Writing $image ($image_size bytes) to 0x$(printf %x $addr) via OpenOCD..."

if pgrep -f "probe-rs" > /dev/null 2>&1; then
    print -u2 -r -- "error: a probe-rs process is running and likely holds the debug probe."
    print -u2 -r -- "       Stop it first (e.g. 'pkill -f probe-rs'), then re-run."
    exit 1
fi

"$OPENOCD" \
    -f interface/cmsis-dap.cfg \
    -c "transport select swd" \
    -f target/stm32f4x.cfg \
    -f "$repo_root/scripts/sflash_write_swd.tcl" \
    -c "sflash_stub_elf {$STUB_ELF}" \
    -c "sflash_write_file {$image} $addr" \
    -c "shutdown"

print -r -- "OpenOCD session done; resetting target back to the normal application..."
probe-rs reset --chip "$CHIP" --protocol swd --speed "$SPEED" --non-interactive

print -r -- "Waiting for device to rejoin Wi-Fi at $REWAIR_IP..."
verify_ok=0
for _ in {1..15}; do
    sleep 2
    if curl -sf -m 3 "http://$REWAIR_IP/api/status" > /dev/null 2>&1; then
        verify_ok=1
        break
    fi
done

if (( ! verify_ok )); then
    print -u2 -r -- "device did not come back on the network at $REWAIR_IP; skipping HTTP readback verification"
    print -u2 -r -- "(the write itself succeeded per OpenOCD's POST_WRITE_VERIFY -- only network-side confirmation is unavailable)"
    exit 2
fi

# Sample a few offsets across the written image rather than the whole thing
# (the debug route caps len<=256 per request).
print -r -- "Verifying readback via /api/debug/sflash..."
sample_offsets=(0)
if (( image_size > 256 )); then
    sample_offsets+=($(( image_size / 2 )))
fi
if (( image_size > 32 )); then
    sample_offsets+=($(( image_size - 32 )))
fi

fail=0
for off in "${sample_offsets[@]}"; do
    remaining=$(( image_size - off ))
    len=$(( remaining < 256 ? remaining : 256 ))
    (( len == 0 )) && continue
    sample_addr=$(( addr + off ))
    expected_hex=$(dd if="$image" bs=1 skip=$off count=$len 2>/dev/null | xxd -p | tr -d '\n')
    got_json=$(curl -sf -m 5 "http://$REWAIR_IP/api/debug/sflash?addr=$(printf %x $sample_addr)&len=$len")
    got_hex=$(print -r -- "$got_json" | sed -n 's/.*"data":"\([0-9a-f]*\)".*/\1/p')
    if [[ "$got_hex" == "$expected_hex" ]]; then
        print -r -- "ok   offset $off (addr 0x$(printf %x $sample_addr), $len bytes) matches"
    else
        print -u2 -r -- "FAIL offset $off (addr 0x$(printf %x $sample_addr), $len bytes) mismatch"
        print -u2 -r -- "  expected: $expected_hex"
        print -u2 -r -- "  got:      $got_hex"
        fail=1
    fi
done

if (( fail )); then
    print -u2 -r -- "readback verification FAILED"
    exit 1
fi

print -r -- "Write + readback verification OK: $image -> 0x$(printf %x $addr) ($image_size bytes)"
