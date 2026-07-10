#!/usr/bin/env zsh
set -euo pipefail

CHIP="${CHIP:-STM32F411CE}"
SPEED="${SPEED:-950}"

if [[ -z "${IMAGE:-}" ]]; then
  print -u2 -r -- "Set IMAGE=/path/to/your-stock-boot-sector-backup.bin"
  exit 2
fi

probe-rs download \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive \
  --verify \
  --binary-format bin \
  --base-address 0x08000000 \
  "$IMAGE"

probe-rs reset \
  --chip "$CHIP" \
  --protocol swd \
  --speed "$SPEED" \
  --non-interactive
