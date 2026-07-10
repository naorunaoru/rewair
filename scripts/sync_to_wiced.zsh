#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
RWFS_IMAGE="$repo_root/webui/webui.rwfs"

if [[ ! -d "$SDK_DIR" ]]; then
  print -u2 -r -- "WICED SDK not found: $SDK_DIR"
  print -u2 -r -- "Set SDK_DIR=/path/to/wiced-emw3165"
  exit 1
fi

if [[ ! -f "$RWFS_IMAGE" && "${REQUIRE_RWFS:-0}" == "1" ]]; then
  print -u2 -r -- "Embedded web UI image not found: $RWFS_IMAGE"
  print -u2 -r -- "Run 'cd webui && npm run build' first."
  exit 1
fi

rsync -a --delete "$repo_root/wiced/apps/rewair/" "$SDK_DIR/apps/rewair/"
rsync -a --delete "$repo_root/wiced/platforms/AWAIR/" "$SDK_DIR/platforms/AWAIR/"
if [[ -f "$RWFS_IMAGE" ]]; then
  cp "$RWFS_IMAGE" "$SDK_DIR/apps/rewair/local_bridge/webui.bin"
fi

print -r -- "Synced Rewair app/platform into $SDK_DIR"
if [[ -f "$RWFS_IMAGE" ]]; then
  print -r -- "Synced embedded RWFS into $SDK_DIR/apps/rewair/local_bridge/webui.bin"
fi
