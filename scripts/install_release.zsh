#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
BACKUP_DIR="${BACKUP_DIR:-}"
VERIFY_ONLY="${VERIFY_ONLY:-0}"

if [[ $# -ne 1 ]]; then
  print -u2 -r -- "usage: BACKUP_DIR=/path/to/stock-backup $0 <release-directory>"
  exit 2
fi

release_dir="${1:A}"
if [[ ! -d "$release_dir" ]]; then
  print -u2 -r -- "release directory not found: $release_dir"
  exit 2
fi

for tool in probe-rs awk shasum; do
  if ! command -v "$tool" > /dev/null 2>&1; then
    print -u2 -r -- "required tool not found: $tool"
    exit 127
  fi
done
if [[ -z "${OPENOCD:-}" ]] && ! command -v openocd > /dev/null 2>&1; then
  print -u2 -r -- "OpenOCD not found; install it or set OPENOCD=/path/to/openocd"
  exit 127
fi

if [[ -z "$BACKUP_DIR" ]]; then
  print -u2 -r -- "Set BACKUP_DIR to the Step 0 backup directory before installing."
  exit 2
fi

backup_internal="$BACKUP_DIR/f411-internal-flash.bin"
backup_external="$BACKUP_DIR/external-spi-flash.bin"
if [[ ! -f "$backup_internal" || ! -f "$backup_external" ]]; then
  print -u2 -r -- "BACKUP_DIR must contain f411-internal-flash.bin and external-spi-flash.bin"
  exit 2
fi

internal_size="$(wc -c < "$backup_internal" | tr -d ' ')"
external_size="$(wc -c < "$backup_external" | tr -d ' ')"
if [[ "$internal_size" != 524288 || "$external_size" != 2097152 ]]; then
  print -u2 -r -- "Step 0 backup has an unexpected size; refusing to install"
  exit 1
fi

assets=(
  rewair-bootloader.bin
  rewair-application.bin
  rewair-sflash.bin
  rewair-sflash-loader.bin
)
checksums="$release_dir/SHA256SUMS.txt"
if [[ ! -f "$checksums" ]]; then
  print -u2 -r -- "release asset not found: $checksums"
  exit 2
fi

for asset in "${assets[@]}"; do
  asset_path="$release_dir/$asset"
  if [[ ! -f "$asset_path" ]]; then
    print -u2 -r -- "release asset not found: $asset_path"
    exit 2
  fi
  expected="$(awk -v target="$asset" '$2 == target { print $1 }' "$checksums")"
  actual="$(shasum -a 256 -- "$asset_path" | awk '{ print $1 }')"
  if [[ -z "$expected" || "$actual" != "$expected" ]]; then
    print -u2 -r -- "checksum verification failed: $asset"
    exit 1
  fi
  print -r -- "verified $asset"
done

if [[ "$VERIFY_ONLY" == 1 ]]; then
  print -r -- "Release assets and Step 0 backup verified."
  exit 0
fi

STUB_IMAGE="$release_dir/rewair-sflash-loader.bin" \
  "$repo_root/tools/recovery/stock_sflash.zsh" \
  write "$release_dir/rewair-sflash.bin" 0x101000

download_bin() {
  local image="$1" address="$2"
  probe-rs download \
    --chip "$CHIP" \
    --protocol swd \
    --speed "$SPEED" \
    --non-interactive \
    --verify \
    --binary-format bin \
    --base-address "$address" \
    "$image"
}

download_bin "$release_dir/rewair-bootloader.bin" 0x08000000
download_bin "$release_dir/rewair-application.bin" 0x0800c000

probe-rs reset \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive

print -r -- "Rewair installation complete. DCT and saved device data were preserved."
