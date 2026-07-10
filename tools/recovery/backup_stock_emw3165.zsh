#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -ne 1 ]]; then
  print -u2 -r -- "usage: $0 <output-directory>"
  exit 2
fi

script_dir="${0:A:h}"
output_dir="$1"
internal="$output_dir/f411-internal-flash.bin"
external="$output_dir/external-spi-flash.bin"

if [[ -e "$internal" || -e "$external" ]]; then
  print -u2 -r -- "refusing to overwrite an existing backup in: $output_dir"
  exit 2
fi

mkdir -p -- "$output_dir"

"$script_dir/backup_stock_f411.zsh" "$internal"
"$script_dir/stock_sflash.zsh" read "$external"

print -r -- "Stock EMW3165 backup complete: $output_dir"
shasum -a 256 -- "$internal" "$external"
