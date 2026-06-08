#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"

if [[ ! -d "$SDK_DIR" ]]; then
  print -u2 -r -- "WICED SDK not found: $SDK_DIR"
  print -u2 -r -- "Set SDK_DIR=/path/to/wiced-emw3165"
  exit 1
fi

rsync -a --delete "$repo_root/wiced/apps/rewair/" "$SDK_DIR/apps/rewair/"
rsync -a --delete "$repo_root/wiced/platforms/AWAIR/" "$SDK_DIR/platforms/AWAIR/"

print -r -- "Synced Rewair app/platform into $SDK_DIR"
