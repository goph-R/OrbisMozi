#!/bin/sh
# Builds libsmb2 (SMB2/SMB3 client, used to play files from network shares)
# as a static library, once for the PS4 and once for the PC.
#
#   tools/build-libsmb2.sh [toolchain-dir]
#
# toolchain-dir defaults to ../PS4-Toolchain and must contain
#   OpenOrbis/PS4Toolchain      the unpacked OpenOrbis release
# and will receive
#   libsmb2-src/  libsmb2-ps4/  libsmb2-host/
#
# Needs: clang, cmake, git, llvm-ar (or llvm-ar-NN).
set -eu

LIBSMB2_TAG=libsmb2-7.0.0
ROOT=$(cd "${1:-$(dirname "$0")/../../PS4-Toolchain}" && pwd)
TC="$ROOT/OpenOrbis/PS4Toolchain"
[ -f "$TC/link.x" ] || { echo "OpenOrbis toolchain not found in $TC" >&2; exit 1; }

AR=""
for t in llvm-ar llvm-ar-18 llvm-ar-19 llvm-ar-17; do
    if command -v "$t" >/dev/null 2>&1; then AR=$(command -v "$t"); break; fi
done
[ -n "$AR" ] || { echo "missing tool: llvm-ar" >&2; exit 1; }
JOBS=$(nproc 2>/dev/null || echo 4)

SRC="$ROOT/libsmb2-src"
[ -f "$SRC/CMakeLists.txt" ] || \
    git clone --depth 1 --branch "$LIBSMB2_TAG" https://github.com/sahlberg/libsmb2.git "$SRC"

# Only the SMB client itself: no Kerberos, no tools, static.
COMMON="-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF
        -DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF -DCMAKE_C_COMPILER=clang"

install_lib() {                   # build-dir prefix
    mkdir -p "$2/lib" "$2/include"
    cp "$1/lib/libsmb2.a" "$2/lib/"
    rm -rf "$2/include/smb2"
    cp -r "$SRC/include/smb2" "$2/include/"
}

# --- PS4 ---------------------------------------------------------------
# - Nothing can be linked while configuring, so cmake only test-compiles.
# - The toolchain's <endian.h> has __bswapNN as functions; libsmb2 would
#   otherwise redefine them to names that do not exist.
# - getrandom() is declared in the headers but is not a PS4 system call;
#   switching it off makes libsmb2 use /dev/urandom or random() instead.
PS4_CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -march=btver2 -isysroot $TC -isystem $TC/include"
PS4_CFLAGS="$PS4_CFLAGS -DPS4_PLATFORM -D__bswap16=__bswap16 -D__bswap32=__bswap32 -D__bswap64=__bswap64 -Wno-#warnings"

# shellcheck disable=SC2086
cmake -S "$SRC" -B "$ROOT/libsmb2-ps4-build" $COMMON \
    -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_AR="$AR" -DCMAKE_RANLIB=/bin/true \
    -DHAVE_GETRANDOM=OFF -DHAVE_ARC4RANDOM_BUF=OFF \
    -DCMAKE_C_FLAGS="$PS4_CFLAGS"
cmake --build "$ROOT/libsmb2-ps4-build" -j"$JOBS" --target smb2
install_lib "$ROOT/libsmb2-ps4-build" "$ROOT/libsmb2-ps4"

# --- PC (for `make host`) ------------------------------------------------
# shellcheck disable=SC2086
cmake -S "$SRC" -B "$ROOT/libsmb2-host-build" $COMMON
cmake --build "$ROOT/libsmb2-host-build" -j"$JOBS" --target smb2
install_lib "$ROOT/libsmb2-host-build" "$ROOT/libsmb2-host"

echo "libsmb2 installed to $ROOT/libsmb2-ps4 and $ROOT/libsmb2-host"
