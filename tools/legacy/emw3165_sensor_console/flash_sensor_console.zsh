#!/usr/bin/env zsh
set -euo pipefail

CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
script_dir="${0:A:h}"
ELF="${ELF:-$script_dir/build/emw3165_sensor_console.elf}"

probe-rs download \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive \
  --verify \
  "$ELF"

probe-rs reset \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive
