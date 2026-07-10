#!/usr/bin/env zsh
set -euo pipefail

repo_root="${0:A:h:h}"
SDK_DIR="${SDK_DIR:-$repo_root/../third_party/wiced-emw3165}"
OUTPUT_DIR="${OUTPUT_DIR:-$repo_root/build/release}"
VERSION="${VERSION:-dev}"
SOURCE_REVISION="${SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"
SDK_REVISION="${SDK_REVISION:-$(git -C "$SDK_DIR" rev-parse HEAD)}"

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

"$WICED_MAKE" -C "$SDK_DIR" waf.sflash_write-NoOS-NoNS-AWAIR "HOST_OS=$HOST_OS"
rm -rf "$OUTPUT_DIR"
node "$repo_root/scripts/package_release.mjs" \
  "$SDK_DIR" "$OUTPUT_DIR" "$VERSION" "$SOURCE_REVISION" "$SDK_REVISION"

print -r -- "Release bundle: $OUTPUT_DIR"
