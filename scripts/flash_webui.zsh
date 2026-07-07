#!/usr/bin/env zsh
# Builds the web UI (webui/) with Vite + the RWFS packer, then writes the
# resulting webui/webui.rwfs image to the UI region of external SPI flash
# (0x1C0000) via scripts/flash_sflash_openocd.zsh.
#
# Usage:
#   scripts/flash_webui.zsh
#
# Requires: node/npm (webui/ build), and everything
# scripts/flash_sflash_openocd.zsh requires (OpenOCD + a free debug probe;
# stop any running probe-rs session first).
set -euo pipefail

repo_root="${0:A:h:h}"
WEBUI_DIR="$repo_root/webui"
RWFS_IMAGE="$WEBUI_DIR/webui.rwfs"
UI_REGION_ADDR="0x1C0000"

if [[ ! -d "$WEBUI_DIR" ]]; then
    print -u2 -r -- "webui/ not found at $WEBUI_DIR"
    exit 1
fi

print -r -- "Building web UI (npm run build in $WEBUI_DIR)..."
( cd "$WEBUI_DIR" && npm run build )

if [[ ! -f "$RWFS_IMAGE" ]]; then
    print -u2 -r -- "build did not produce $RWFS_IMAGE"
    exit 1
fi

rwfs_size=$(wc -c < "$RWFS_IMAGE" | tr -d ' ')
print -r -- "Built $RWFS_IMAGE ($rwfs_size bytes)"

print -r -- "Flashing $RWFS_IMAGE to $UI_REGION_ADDR..."
"$repo_root/scripts/flash_sflash_openocd.zsh" "$RWFS_IMAGE" "$UI_REGION_ADDR"
