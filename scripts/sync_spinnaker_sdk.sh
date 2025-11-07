#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ADDON_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDK_ROOT="${1:-/Applications/Spinnaker}"

if [[ ! -d "${SDK_ROOT}" ]]; then
    echo "[ofxSpinnaker] Spinnaker SDK not found at: ${SDK_ROOT}" >&2
    echo "Usage: ${BASH_SOURCE[0]} /path/to/Spinnaker" >&2
    exit 1
fi

echo "[ofxSpinnaker] Syncing headers from ${SDK_ROOT}/include"
rsync -a --delete "${SDK_ROOT}/include/" "${ADDON_ROOT}/libs/spinnaker/include/"

OSX_LIB_SRC="${SDK_ROOT}/lib"
OSX_LIB_DST="${ADDON_ROOT}/libs/spinnaker/lib/osx"

if [[ -d "${OSX_LIB_SRC}" ]]; then
    echo "[ofxSpinnaker] Syncing macOS libraries"
    rsync -a --delete \
        --include "*/" \
        --include "*.dylib" \
        --include "*.dylib.*" \
        --exclude "*" \
        "${OSX_LIB_SRC}/" "${OSX_LIB_DST}/"

    if [[ -d "${OSX_LIB_SRC}/spinnaker-gentl" ]]; then
        mkdir -p "${OSX_LIB_DST}/flir-gentl"
        rsync -a --delete "${OSX_LIB_SRC}/spinnaker-gentl/" "${OSX_LIB_DST}/flir-gentl/"
    fi
fi

echo "[ofxSpinnaker] Sync complete."

