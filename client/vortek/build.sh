#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "${ROOTFS:-}" ]; then
    if [ -n "${PREFIX:-}" ] && [ -d "$PREFIX/glibc" ]; then
        ROOTFS="$PREFIX/glibc"
    else
        ROOTFS="/data/data/com.termux/files/usr/glibc"
    fi
fi

export ROOTFS
export CFLAGS="${CFLAGS:-"-O2 -Wl,-rpath=$ROOTFS/lib"}"

cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build" -DCMAKE_INSTALL_PREFIX="$ROOTFS" -DCMAKE_C_FLAGS_RELEASE="$CFLAGS" -DCMAKE_BUILD_TYPE=Release
cmake --build "$SCRIPT_DIR/build" -j"${JOBS:-8}"
cmake --install "$SCRIPT_DIR/build"

if [ -f "$SCRIPT_DIR/create-asset.sh" ]; then
    bash "$SCRIPT_DIR/create-asset.sh"
fi

if [ "${BUILD_BCN_LAYER:-0}" = "1" ]; then
    ROOTFS="$ROOTFS" "$SCRIPT_DIR/../bcn_layer/build.sh"
fi
