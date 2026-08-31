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
mkdir -p "${ADDON_ROOT}/libs/spinnaker/include"
rsync -a --delete "${SDK_ROOT}/include/" "${ADDON_ROOT}/libs/spinnaker/include/"

echo "[ofxSpinnaker] Patching ambiguous includes"
# Interface/IImageList.h and Interface/IImageStatistics.h include "Image.h" by
# bare filename. Because openFrameworks' bundled ANGLE library also ships a
# file literally named Image.h (libs/metalangle/include/src/libANGLE/Image.h),
# and OF's core include paths are always searched before an addon's own
# (config.project.mk builds PROJECT_INCLUDE_CFLAGS as core, then project, then
# addon includes - an addon can't reorder that), the bare include resolves to
# the wrong file when built inside an OF project. Rewrite it to an explicit
# relative path so it can't be shadowed regardless of search-path order.
INCLUDE_DIR="${ADDON_ROOT}/libs/spinnaker/include"
for f in "${INCLUDE_DIR}/Interface/IImageList.h" "${INCLUDE_DIR}/Interface/IImageStatistics.h"; do
    if [[ -f "${f}" ]]; then
        sed -i '' 's#include "Image\.h"#include "../Image.h"#' "${f}"
    fi
done

OSX_LIB_SRC="${SDK_ROOT}/lib"
OSX_LIB_DST="${ADDON_ROOT}/libs/spinnaker/lib/osx"

if [[ -d "${OSX_LIB_SRC}" ]]; then
    echo "[ofxSpinnaker] Syncing macOS libraries"
    mkdir -p "${OSX_LIB_DST}"
    # openFrameworks auto-links and bundles EVERY .dylib it finds under an
    # addon's libs/*/lib/<platform>/ folder - addon_config.mk's ADDON_LIBS
    # cannot narrow that down (it only ever adds to that auto-discovered list,
    # never removes from it). So the allowlist below IS the mechanism that
    # controls what actually gets linked into the app: it's the real,
    # confirmed dependency closure of Spinnaker/Spinnaker_C/SpinUpdate,
    # excluding SpinVideo (unused, pulls in a full ffmpeg chain) and the
    # bundled CppUnit/GCBaseTest dev-only test libraries.
    rsync -a --delete \
        --exclude "flir-gentl/" \
        --include "*/" \
        --include "libSpinnaker.*.dylib" \
        --include "libSpinnaker_C.*.dylib" \
        --include "libSpinUpdate.*.dylib" \
        --include "libGenApi_*.dylib" \
        --include "libGCBase_*.dylib" \
        --include "libNodeMapData_*.dylib" \
        --include "libMathParser_*.dylib" \
        --include "libXmlParser_*.dylib" \
        --include "liblog4cpp_*.dylib" \
        --include "libLog_*.dylib" \
        --exclude "*" \
        "${OSX_LIB_SRC}/" "${OSX_LIB_DST}/"

    if [[ -d "${OSX_LIB_SRC}/spinnaker-gentl" ]]; then
        mkdir -p "${OSX_LIB_DST}/flir-gentl"
        rsync -a --delete "${OSX_LIB_SRC}/spinnaker-gentl/" "${OSX_LIB_DST}/flir-gentl/"
    fi

    # libSpinnaker/libSpinUpdate depend on Homebrew's libomp and libusb by
    # absolute path (/opt/homebrew/...), which the SDK itself does not ship
    # under lib/ - only bundled inside its SpinView app. Pull those two copies
    # in from there so the addon never depends on anything being installed
    # system-wide; the patch step below rewrites the absolute references to
    # point at these bundled copies instead.
    SPINVIEW_FRAMEWORKS="${SDK_ROOT}/apps/SpinView_QT.app/Contents/Frameworks"
    if [[ -d "${SPINVIEW_FRAMEWORKS}" ]]; then
        echo "[ofxSpinnaker] Bundling libomp/libusb from SpinView"
        for dep in libomp.dylib libusb-1.0.0.dylib; do
            if [[ -f "${SPINVIEW_FRAMEWORKS}/${dep}" ]]; then
                cp -f "${SPINVIEW_FRAMEWORKS}/${dep}" "${OSX_LIB_DST}/${dep}"
            fi
        done
    fi

    echo "[ofxSpinnaker] Creating toolchain-agnostic symlinks"
    # The GenICam support libraries are shipped with a toolchain-specific suffix
    # (e.g. libGenApi_clang140_v3_0.dylib) that changes between SDK releases.
    # addon_config.mk links against the stable names below so it doesn't need to
    # be edited every time the SDK is upgraded to a build with a new suffix.
    for stem in GenApi GCBase NodeMapData MathParser XmlParser log4cpp Log; do
        match="$(find "${OSX_LIB_DST}" -maxdepth 1 -name "lib${stem}_*.dylib" | sort | head -n 1)"
        if [[ -n "${match}" ]]; then
            ln -sf "$(basename "${match}")" "${OSX_LIB_DST}/lib${stem}.dylib"
        else
            echo "[ofxSpinnaker] Warning: no lib${stem}_*.dylib found in ${OSX_LIB_DST}" >&2
        fi
    done

    echo "[ofxSpinnaker] Making library dependencies self-contained"
    # The addon copies these dylibs next to the built app and relies on
    # @executable_path/@loader_path rpaths to find them - it does not assume
    # /Applications/Spinnaker, /usr/local, or Homebrew are present on the
    # machine actually running the app. Rewrite every dylib's (and the GenTL
    # producer's) own ID and any absolute /opt/homebrew or /usr/local
    # dependency path to @rpath so that holds true, then re-sign (required on
    # Apple Silicon after an install_name_tool edit invalidates the existing
    # signature). The GenTL producer (flir-gentl/Spinnaker_GenTL.cti) needs
    # this too: Spinnaker's System::GetInstance() dlopen()s it directly, and
    # it depends on libusb by the same absolute Homebrew path.
    patch_dependencies() {
        local f="$1"
        local base
        base="$(basename "${f}")"
        install_name_tool -id "@rpath/${base}" "${f}" 2>/dev/null || true

        local dep
        while IFS= read -r dep; do
            case "${dep}" in
                /opt/homebrew/*|/usr/local/*)
                    install_name_tool -change "${dep}" "@rpath/$(basename "${dep}")" "${f}" 2>/dev/null || true
                    ;;
            esac
        done < <(otool -L "${f}" | tail -n +2 | awk '{print $1}')

        codesign --force -s - "${f}" 2>/dev/null || true
    }

    while IFS= read -r -d '' f; do
        patch_dependencies "${f}"
    done < <(find "${OSX_LIB_DST}" -maxdepth 1 -type f -name "*.dylib*" -print0)

    if [[ -f "${OSX_LIB_DST}/flir-gentl/Spinnaker_GenTL.cti" ]]; then
        patch_dependencies "${OSX_LIB_DST}/flir-gentl/Spinnaker_GenTL.cti"
    fi
fi

echo "[ofxSpinnaker] Sync complete."

