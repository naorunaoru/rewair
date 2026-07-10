#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
TARGET="${TARGET:-rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
BUILD_NAME="${BUILD_NAME:-rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
WEBUI_DIR="$repo_root/webui"

if [[ -z "${HOST_OS:-}" ]]; then
  case "$(uname -s)-$(uname -m)" in
    Darwin-*) HOST_OS=OSX ;;
    Linux-x86_64|Linux-amd64) HOST_OS=Linux64 ;;
    Linux-i?86) HOST_OS=Linux32 ;;
    *)
      print -u2 -r -- "Unsupported build host: $(uname -s) $(uname -m); set HOST_OS explicitly"
      exit 1
      ;;
  esac
fi
WICED_MAKE="${WICED_MAKE:-$SDK_DIR/tools/common/$HOST_OS/make}"

if [[ ! -x "$WICED_MAKE" ]]; then
  print -u2 -r -- "WICED make not found: $WICED_MAKE"
  exit 1
fi

if [[ ! -x "$WEBUI_DIR/node_modules/.bin/vite" ]]; then
  print -r -- "Installing web UI dependencies (npm ci in $WEBUI_DIR)..."
  ( cd "$WEBUI_DIR" && npm ci )
fi

print -r -- "Building embedded web UI (npm run build in $WEBUI_DIR)..."
( cd "$WEBUI_DIR" && npm run build )

REQUIRE_RWFS=1 "$repo_root/scripts/sync_to_wiced.zsh"

cd "$SDK_DIR"
"$WICED_MAKE" "$TARGET" "HOST_OS=$HOST_OS"

# A normal WICED build does not materialize APPS.bin unless its legacy
# download_apps target is requested.  Build just the lookup-table artifact;
# flashing is deliberately handled by our CMSIS-DAP/SWD script instead of
# the SDK's old JTAG-only downloader.
"$WICED_MAKE" -r -f tools/makefiles/wiced_elf.mk \
  "build/$BUILD_NAME/APPS.bin" \
  CLEANED_BUILD_STRING="$BUILD_NAME" \
  DIR_BUILD_STRING="$BUILD_NAME" \
  "HOST_OS=$HOST_OS" SOURCE_ROOT=./ MAKEFILES_PATH=tools/makefiles

app_bin="$SDK_DIR/build/$BUILD_NAME/binary/$BUILD_NAME.bin"
app_limit=$(( 0x74000 ))
app_size=$(wc -c < "$app_bin" | tr -d ' ')
if (( app_size > app_limit )); then
  print -u2 -r -- "application image is $app_size bytes; limit is $app_limit bytes"
  exit 1
fi
print -r -- "Built firmware + embedded RWFS: $app_bin ($app_size / $app_limit bytes)"
