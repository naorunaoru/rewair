#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"

CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
BUILD_NAME="${BUILD_NAME:-rewair_bi201_probe-AWAIR-FreeRTOS-LwIP-SDIO}"
APP_ELF="${APP_ELF:-$SDK_DIR/build/$BUILD_NAME/binary/$BUILD_NAME.elf}"

if [[ ! -f "$APP_ELF" ]]; then
  print -u2 -r -- "BI201 probe ELF not found: $APP_ELF"
  print -u2 -r -- "Run scripts/build_bi201_probe.zsh first."
  exit 1
fi

# Flash only the application slot.  The existing bootloader and DCT (including
# saved Wi-Fi credentials) are intentionally left untouched.
probe-rs download \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive \
  --verify \
  "$APP_ELF"

probe-rs reset \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive
