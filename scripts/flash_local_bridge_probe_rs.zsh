#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"

CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
BUILD_NAME="${BUILD_NAME:-rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
BUILD_DIR="${BUILD_DIR:-$SDK_DIR/build/$BUILD_NAME}"
BOOT_ELF="${BOOT_ELF:-$SDK_DIR/build/waf_bootloader-NoOS-NoNS-AWAIR-SDIO/binary/waf_bootloader-NoOS-NoNS-AWAIR-SDIO.elf}"
DCT_ELF="${DCT_ELF:-$BUILD_DIR/DCT.elf}"
APP_ELF="${APP_ELF:-$BUILD_DIR/binary/$BUILD_NAME.elf}"
FLASH_DCT="${FLASH_DCT:-0}"

download_elf() {
  local image="$1"
  probe-rs download \
    --chip "$CHIP" \
    --protocol swd \
    --speed "$SPEED" \
    --non-interactive \
    --verify \
    "$image"
}

if [[ ! -f "$APP_ELF" ]]; then
  print -u2 -r -- "App ELF not found: $APP_ELF"
  print -u2 -r -- "Run scripts/build_local_bridge.zsh first."
  exit 1
fi

download_elf "$BOOT_ELF"
if [[ "$FLASH_DCT" == "1" ]]; then
  download_elf "$DCT_ELF"
else
  print "Skipping DCT flash; set FLASH_DCT=1 for a clean DCT/factory credential reset"
fi
download_elf "$APP_ELF"

probe-rs reset \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive
