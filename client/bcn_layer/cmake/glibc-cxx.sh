#!/bin/sh
set -eu

rootfs="${ROOTFS:-/data/data/com.termux/files/usr/glibc}"
compiler="${VORTEK_GLIBC_CXX:-$rootfs/bin/c++}"
runner="${GRUN:-grun}"

exec "$runner" "$compiler" "$@"
