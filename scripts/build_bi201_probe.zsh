#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
TARGET="${TARGET:-rewair.bi201_probe-AWAIR-FreeRTOS-LwIP-SDIO}"

"$repo_root/scripts/sync_to_wiced.zsh"

cd "$SDK_DIR"
tools/common/OSX/make "$TARGET" HOST_OS=OSX
