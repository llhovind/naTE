#!/usr/bin/env bash
# scripts/build-appimage.sh — configure → build → install → AppImage
# Usage: ./scripts/build-appimage.sh [--skip-configure]
#
# Requirements: cmake, a C++ toolchain, GTK3/libssh2/wxWidgets dev headers.
# linuxdeploy and linuxdeploy-plugin-gtk are downloaded automatically if not
# found in PATH or the project root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-appimage"
APP_DIR="${BUILD_DIR}/AppDir"
OUTPUT_APPIMAGE="${PROJECT_ROOT}/naTE-x86_64.AppImage"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_GTK_URL="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"

find_or_download() {
    local name="$1"
    local url="$2"
    local dest="${PROJECT_ROOT}/${name}"
    if command -v "${name}" &>/dev/null; then echo "${name}"; return; fi
    if [[ -x "${dest}" ]];              then echo "${dest}";  return; fi
    echo "Downloading ${name}..." >&2
    curl -fsSL --retry 3 --retry-delay 2 -o "${dest}" "${url}"
    chmod +x "${dest}"
    echo "${dest}"
}

LINUXDEPLOY="$(find_or_download linuxdeploy-x86_64.AppImage "${LINUXDEPLOY_URL}")"
LINUXDEPLOY_GTK="$(find_or_download linuxdeploy-plugin-gtk.sh "${LINUXDEPLOY_GTK_URL}")"
export PATH="$(dirname "${LINUXDEPLOY_GTK}"):${PATH}"

needs_configure=true
if [[ "${1:-}" == "--skip-configure" ]] && [[ -f "${BUILD_DIR}/build.ninja" || -f "${BUILD_DIR}/Makefile" ]]; then
    needs_configure=false
fi

if $needs_configure; then
    echo "==> Configuring..."
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
fi

echo "==> Building..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo "==> Installing into AppDir (${APP_DIR})..."
rm -rf "${APP_DIR}"
DESTDIR="${APP_DIR}" cmake --install "${BUILD_DIR}" --strip

echo "==> Packaging AppImage..."
export APPIMAGE_EXTRACT_AND_RUN=1
export DEPLOY_GTK_VERSION=3
export OUTPUT="${OUTPUT_APPIMAGE}"
"${LINUXDEPLOY}" --appdir "${APP_DIR}" --plugin gtk --output appimage

echo "==> Done: ${OUTPUT_APPIMAGE}"
