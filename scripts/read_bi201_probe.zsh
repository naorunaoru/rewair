#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"

CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
BUILD_NAME="${BUILD_NAME:-rewair_bi201_probe-AWAIR-FreeRTOS-LwIP-SDIO}"
APP_ELF="${APP_ELF:-$SDK_DIR/build/$BUILD_NAME/binary/$BUILD_NAME.elf}"
MAILBOX_SIZE=1080

if [[ ! -f "$APP_ELF" ]]; then
  print -u2 -r -- "BI201 probe ELF not found: $APP_ELF"
  exit 1
fi

nm_tool="${NM:-arm-none-eabi-nm}"
if ! command -v "$nm_tool" >/dev/null 2>&1; then
  nm_tool="$SDK_DIR/tools/ARM_GNU/bin/OSX/arm-none-eabi-nm"
fi

mailbox_addr="$($nm_tool -g --defined-only "$APP_ELF" | awk '$3 == "bi201_probe_mailbox" { print "0x" $1; exit }')"
if [[ -z "$mailbox_addr" ]]; then
  print -u2 -r -- "bi201_probe_mailbox symbol not found in $APP_ELF"
  exit 1
fi

tmp="$(mktemp -t bi201-mailbox.XXXXXX)"
trap 'rm -f "$tmp"' EXIT

probe-rs read b8 "$mailbox_addr" "$MAILBOX_SIZE" \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive \
  --output "$tmp" \
  --format binary

print -r -- "mailbox address: $mailbox_addr"
python3 "$repo_root/tools/bi201/decode_mailbox.py" "$tmp"
