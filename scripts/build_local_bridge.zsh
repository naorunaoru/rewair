#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
TARGET="${TARGET:-rewair.local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"
BUILD_NAME="${BUILD_NAME:-rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO}"

"$repo_root/scripts/sync_to_wiced.zsh"

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
