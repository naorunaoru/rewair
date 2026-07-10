#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -ne 1 ]]; then
  print -u2 -r -- "usage: $0 <output.bin>"
  exit 2
fi

output="$1"
speed="${SPEED:-950}"
openocd="${OPENOCD:-}"

if [[ -e "$output" ]]; then
  print -u2 -r -- "refusing to overwrite: $output"
  exit 2
fi

if [[ -z "$openocd" ]]; then
  if command -v openocd > /dev/null 2>&1; then
    openocd="$(command -v openocd)"
  elif [[ -x /opt/homebrew/bin/openocd ]]; then
    openocd=/opt/homebrew/bin/openocd
  elif [[ -x /usr/local/bin/openocd ]]; then
    openocd=/usr/local/bin/openocd
  fi
fi

if [[ -z "$openocd" || ! -x "$openocd" ]]; then
  print -u2 -r -- "OpenOCD not found; install it or set OPENOCD=/path/to/openocd"
  exit 127
fi

mkdir -p -- "${output:h}"
temp="${output}.incomplete.$$"
trap 'rm -f -- "$temp"' EXIT

"$openocd" \
  -f interface/cmsis-dap.cfg \
  -c "transport select swd" \
  -f target/stm32f4x.cfg \
  -c "adapter speed $speed" \
  -c init \
  -c "reset halt" \
  -c "dump_image \"$temp\" 0x08000000 0x80000" \
  -c "reset run" \
  -c shutdown

size="$(wc -c < "$temp" | tr -d ' ')"
if [[ "$size" != 524288 ]]; then
  print -u2 -r -- "unexpected dump size: $size bytes (expected 524288)"
  exit 1
fi

mv -- "$temp" "$output"
trap - EXIT

print -r -- "Saved stock F411 backup: $output"
shasum -a 256 -- "$output"
