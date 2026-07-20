#!/bin/bash
set -e

if [ -z "${ROOTFS:-}" ]; then
    if [ -n "${PREFIX:-}" ] && [ -d "$PREFIX/glibc" ]; then
        ROOTFS="$PREFIX/glibc"
    else
        ROOTFS="/data/data/com.termux/files/usr/glibc"
    fi
fi

export ROOTFS
export CFLAGS="${CFLAGS:-"-O2 -Wl,-rpath=$ROOTFS/lib"}"

cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$ROOTFS" -DCMAKE_C_FLAGS_RELEASE="$CFLAGS" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"${JOBS:-8}"
cmake --install build

if [ -f create-asset.sh ]; then
    bash create-asset.sh
fi
