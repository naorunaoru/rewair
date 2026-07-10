#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
TARGET="${TARGET:-rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
BUILD_NAME="${BUILD_NAME:-rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
WEBUI_DIR="$repo_root/webui"

if [[ ! -x "$WEBUI_DIR/node_modules/.bin/vite" ]]; then
  print -r -- "Installing web UI dependencies (npm ci in $WEBUI_DIR)..."
  ( cd "$WEBUI_DIR" && npm ci )
fi

print -r -- "Building embedded web UI (npm run build in $WEBUI_DIR)..."
( cd "$WEBUI_DIR" && npm run build )

REQUIRE_RWFS=1 "$repo_root/scripts/sync_to_wiced.zsh"

cd "$SDK_DIR"
tools/common/OSX/make "$TARGET" HOST_OS=OSX

# A normal WICED build does not materialize APPS.bin unless its legacy
# download_apps target is requested.  Build just the lookup-table artifact;
# flashing is deliberately handled by our CMSIS-DAP/SWD script instead of
# the SDK's old JTAG-only downloader.
tools/common/OSX/make -r -f tools/makefiles/wiced_elf.mk \
  "build/$BUILD_NAME/APPS.bin" \
  CLEANED_BUILD_STRING="$BUILD_NAME" \
  DIR_BUILD_STRING="$BUILD_NAME" \
  HOST_OS=OSX SOURCE_ROOT=./ MAKEFILES_PATH=tools/makefiles

app_bin="$SDK_DIR/build/$BUILD_NAME/binary/$BUILD_NAME.bin"
app_limit=$(( 0x74000 ))
app_size=$(wc -c < "$app_bin" | tr -d ' ')
if (( app_size > app_limit )); then
  print -u2 -r -- "application image is $app_size bytes; limit is $app_limit bytes"
  exit 1
fi
print -r -- "Built firmware + embedded RWFS: $app_bin ($app_size / $app_limit bytes)"
