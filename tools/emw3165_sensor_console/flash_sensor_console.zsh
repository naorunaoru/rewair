#!/usr/bin/env zsh
set -euo pipefail

CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"
repo_root="${0:A:h:h:h}"
ELF="${ELF:-$repo_root/tools/emw3165_sensor_console/build/emw3165_sensor_console.elf}"

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
