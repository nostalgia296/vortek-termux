#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "${ROOTFS:-}" ]; then
    if [ -n "${PREFIX:-}" ] && [ -d "$PREFIX/glibc" ]; then
        ROOTFS="$PREFIX/glibc"
    else
        ROOTFS="/data/data/com.termux/files/usr/glibc"
    fi
fi

for tool in glslc slangc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing $tool. Install shaderc and shader-slang in Termux first." >&2
        exit 1
    fi
done

if [ ! -x "$ROOTFS/bin/c++" ]; then
    echo "Missing glibc C++ compiler: $ROOTFS/bin/c++" >&2
    exit 1
fi

if ! command -v grun >/dev/null 2>&1; then
    echo "Missing grun; it is required to execute the glibc compiler from Termux." >&2
    exit 1
fi

export ROOTFS
export GRUN="$(command -v grun)"
export VORTEK_GLIBC_CXX="${CXX:-$ROOTFS/bin/c++}"
export PATH="$ROOTFS/bin:$PATH"

BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-glibc}"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DCMAKE_CXX_COMPILER="$SCRIPT_DIR/cmake/glibc-cxx.sh" \
    -DCMAKE_INSTALL_PREFIX="$ROOTFS"
cmake --build "$BUILD_DIR" -j"${JOBS:-8}"
cmake --install "$BUILD_DIR"
