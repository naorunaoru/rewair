#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -ne 1 ]]; then
  print -u2 -r -- "usage: $0 <backup-directory>"
  exit 2
fi

script_dir="${0:A:h}"
backup_dir="$1"
internal="$backup_dir/f411-internal-flash.bin"
external="$backup_dir/external-spi-flash.bin"

for image in "$internal" "$external"; do
  if [[ ! -f "$image" ]]; then
    print -u2 -r -- "backup image not found: $image"
    exit 2
  fi
done

internal_size="$(wc -c < "$internal" | tr -d ' ')"
external_size="$(wc -c < "$external" | tr -d ' ')"
if [[ "$internal_size" != 524288 || "$external_size" != 2097152 ]]; then
  print -u2 -r -- "backup has an unexpected size; refusing to restore"
  exit 1
fi

"$script_dir/stock_sflash.zsh" write "$external"
IMAGE="$internal" "$script_dir/restore_stock_f411.zsh"

print -r -- "Stock EMW3165 internal and external flash restored from: $backup_dir"
