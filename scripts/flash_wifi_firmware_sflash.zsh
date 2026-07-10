#!/usr/bin/env zsh
# Provision and byte-verify the WICED apps lookup table and BCM43362 firmware
# in the EMW3165's external MX25L1606E over the connected CMSIS-DAP probe.
# The exact target region is backed up before either write.
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
BUILD_NAME="${BUILD_NAME:-rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
OPENOCD="${OPENOCD:-}"
CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
BACKUP_DIR="${BACKUP_DIR:-/tmp/rewair-wifi-relocation-$(date +%Y%m%d-%H%M%S)}"

LUT_ADDR=0x101000
LUT_SIZE=0x1000
FW_ADDR=0x102000
FW_SLOT_SIZE=0x34000
TOTAL_SIZE=0x35000

lut="$SDK_DIR/build/$BUILD_NAME/APPS.bin"
firmware="$SDK_DIR/resources/firmware/43362/43362A2.bin"
stub="$SDK_DIR/build/waf_sflash_write-NoOS-NoNS-AWAIR/binary/waf_sflash_write-NoOS-NoNS-AWAIR.elf"
tcl="$repo_root/scripts/sflash_write_swd.tcl"

if [[ -z "$OPENOCD" ]]; then
  if command -v openocd >/dev/null 2>&1; then
    OPENOCD="$(command -v openocd)"
  elif [[ -x /opt/homebrew/opt/open-ocd/bin/openocd ]]; then
    OPENOCD=/opt/homebrew/opt/open-ocd/bin/openocd
  elif [[ -x /usr/local/opt/open-ocd/bin/openocd ]]; then
    OPENOCD=/usr/local/opt/open-ocd/bin/openocd
  fi
fi

for file in "$lut" "$firmware" "$stub" "$tcl"; do
  if [[ ! -f "$file" ]]; then
    print -u2 -r -- "required file not found: $file"
    exit 1
  fi
done
if [[ -z "$OPENOCD" || ! -x "$OPENOCD" ]]; then
  print -u2 -r -- "OpenOCD not found"
  exit 127
fi

lut_size=$(wc -c < "$lut" | tr -d ' ')
fw_size=$(wc -c < "$firmware" | tr -d ' ')
if (( lut_size != LUT_SIZE )); then
  print -u2 -r -- "unexpected APPS.bin size: $lut_size (expected $LUT_SIZE)"
  exit 1
fi
if (( fw_size != 210412 || fw_size > FW_SLOT_SIZE )); then
  print -u2 -r -- "unexpected 43362A2.bin size: $fw_size"
  exit 1
fi

mkdir -p "$BACKUP_DIR"

openocd_base=(
  -f interface/cmsis-dap.cfg
  -c "transport select swd"
  -c "adapter speed $SPEED"
  -f target/stm32f4x.cfg
  -f "$tcl"
  -c "sflash_stub_elf {$stub}"
)

sflash_read_file_host() {
  local output="$1" address="$2" length="$3"
  local log="$BACKUP_DIR/${output:t}.openocd.log"
  if ! "$OPENOCD" "${openocd_base[@]}" \
      -c "sflash_read_file {$output} $address $length" \
      -c shutdown > "$log" 2>&1; then
    tail -80 "$log"
    return 1
  fi
  tail -3 "$log"
}

sflash_write_file_host() {
  local input="$1" address="$2"
  local log="$BACKUP_DIR/${input:t}-write-${address}.openocd.log"
  if ! "$OPENOCD" "${openocd_base[@]}" \
      -c "sflash_write_file {$input} $address" \
      -c shutdown > "$log" 2>&1; then
    tail -80 "$log"
    return 1
  fi
  tail -3 "$log"
}

before="$BACKUP_DIR/sflash-101000-135fff-before.bin"
after="$BACKUP_DIR/sflash-101000-135fff-after.bin"
print -r -- "Backing up external flash 0x101000-0x135fff to $before"
sflash_read_file_host "$before" $LUT_ADDR $TOTAL_SIZE
shasum -a 256 "$before"

print -r -- "Writing WICED lookup table to 0x101000"
sflash_write_file_host "$lut" $LUT_ADDR
print -r -- "Writing BCM43362A2 firmware to 0x102000"
sflash_write_file_host "$firmware" $FW_ADDR

print -r -- "Reading the complete programmed region back"
sflash_read_file_host "$after" $LUT_ADDR $TOTAL_SIZE

dd if="$after" of="$BACKUP_DIR/APPS-readback.bin" bs=1 count=$LUT_SIZE 2>/dev/null
dd if="$after" of="$BACKUP_DIR/43362A2-readback.bin" bs=1 skip=$LUT_SIZE count=$fw_size 2>/dev/null
cmp "$lut" "$BACKUP_DIR/APPS-readback.bin"
cmp "$firmware" "$BACKUP_DIR/43362A2-readback.bin"

print -r -- "Byte-for-byte verification passed"
shasum -a 256 "$lut" "$BACKUP_DIR/APPS-readback.bin"
shasum -a 256 "$firmware" "$BACKUP_DIR/43362A2-readback.bin"
shasum -a 256 "$after"

probe-rs reset --chip "$CHIP" --protocol swd --speed "$SPEED" --non-interactive
print -r -- "Provisioned LUT 0x101000-0x101fff and Wi-Fi firmware 0x102000-0x135fff"
print -r -- "Backup/readback artifacts: $BACKUP_DIR"
