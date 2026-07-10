#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
OPENOCD="${OPENOCD:-}"
SPEED="${SPEED:-950}"
STUB_IMAGE="${STUB_IMAGE:-}"

if [[ $# -lt 2 || $# -gt 3 || ( "$1" != read && "$1" != write ) || ( "$1" == read && $# -ne 2 ) ]]; then
  print -u2 -r -- "usage: $0 read <output.bin>"
  print -u2 -r -- "       $0 write <input.bin> [offset]"
  exit 2
fi

action="$1"
image="$2"
offset="${3:-0}"

if [[ -z "$OPENOCD" ]]; then
  if command -v openocd > /dev/null 2>&1; then
    OPENOCD="$(command -v openocd)"
  elif [[ -x /opt/homebrew/bin/openocd ]]; then
    OPENOCD=/opt/homebrew/bin/openocd
  elif [[ -x /usr/local/bin/openocd ]]; then
    OPENOCD=/usr/local/bin/openocd
  fi
fi

if [[ -z "$OPENOCD" || ! -x "$OPENOCD" ]]; then
  print -u2 -r -- "OpenOCD not found; install it or set OPENOCD=/path/to/openocd"
  exit 127
fi

if [[ -z "$STUB_IMAGE" ]]; then
  candidates=(
    "$PWD/rewair-sflash-loader.bin"
    "$SDK_DIR/build/waf_sflash_write-NoOS-NoNS-AWAIR/binary/waf_sflash_write-NoOS-NoNS-AWAIR.elf"
  )
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      STUB_IMAGE="$candidate"
      break
    fi
  done
fi

if [[ -z "$STUB_IMAGE" || ! -f "$STUB_IMAGE" ]]; then
  print -u2 -r -- "SPI-flash loader not found."
  print -u2 -r -- "Set STUB_IMAGE=/path/to/rewair-sflash-loader.bin from a Rewair release bundle."
  exit 1
fi

if [[ "$action" == read && -e "$image" ]]; then
  print -u2 -r -- "refusing to overwrite: $image"
  exit 2
fi
if [[ "$action" == write && ! -f "$image" ]]; then
  print -u2 -r -- "backup not found: $image"
  exit 2
fi

if [[ "$action" == write ]]; then
  size="$(wc -c < "$image" | tr -d ' ')"
  if [[ $# -eq 2 && "$size" != 2097152 ]]; then
    print -u2 -r -- "unexpected SPI-flash backup size: $size bytes (expected 2097152)"
    exit 1
  fi
  if ! [[ "$offset" =~ '^(0x[0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)$' ]]; then
    print -u2 -r -- "invalid SPI-flash offset: $offset"
    exit 2
  fi
  offset_value=$(( offset ))
  if (( offset_value < 0 || offset_value >= 0x200000 || size > 0x200000 - offset_value )); then
    print -u2 -r -- "image does not fit in external SPI flash at $offset"
    exit 1
  fi
  command="sflash_write_file {$image} $offset"
  print -r -- "Writing $size bytes to external SPI flash at $offset from $image"
else
  mkdir -p -- "${image:h}"
  temp="${image}.incomplete.$$"
  trap 'rm -f -- "$temp"' EXIT
  command="sflash_read_file {$temp} 0 0x200000"
  print -r -- "Backing up 2 MiB external SPI flash to $image"
fi

"$OPENOCD" \
  -f interface/cmsis-dap.cfg \
  -c "transport select swd" \
  -f target/stm32f4x.cfg \
  -f "$repo_root/scripts/sflash_write_swd.tcl" \
  -c "sflash_stub_image {$STUB_IMAGE}" \
  -c "$command" \
  -c "reset run" \
  -c shutdown

if [[ "$action" == read ]]; then
  size="$(wc -c < "$temp" | tr -d ' ')"
  if [[ "$size" != 2097152 ]]; then
    print -u2 -r -- "unexpected SPI-flash backup size: $size bytes (expected 2097152)"
    exit 1
  fi
  mv -- "$temp" "$image"
  trap - EXIT
  print -r -- "Saved external SPI-flash backup: $image"
  shasum -a 256 -- "$image"
fi
