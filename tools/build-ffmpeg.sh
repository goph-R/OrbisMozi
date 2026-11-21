#!/bin/sh
# Builds FFmpeg as static libraries, once for the PS4 and once for the PC
# (the PC build is only used by `make host`).
#
#   tools/build-ffmpeg.sh [toolchain-dir]
#
# toolchain-dir defaults to ../PS4-Toolchain and must contain
#   OpenOrbis/PS4Toolchain      the unpacked OpenOrbis release
# and will receive
#   ffmpeg-src/  ffmpeg-ps4/  ffmpeg-host/
#
# Needs: clang, ld.lld, nasm, llvm-ar (or llvm-ar-NN), make, curl, tar.
set -eu

FFMPEG_VERSION=7.1.1
ROOT=$(cd "${1:-$(dirname "$0")/../../PS4-Toolchain}" && pwd)
TC="$ROOT/OpenOrbis/PS4Toolchain"
[ -f "$TC/link.x" ] || { echo "OpenOrbis toolchain not found in $TC" >&2; exit 1; }

find_tool() {                     # first existing candidate wins
    for t in "$@"; do
        if command -v "$t" >/dev/null 2>&1; then command -v "$t"; return; fi
        if [ -x "$t" ]; then echo "$t"; return; fi
    done
    echo "missing tool: $1" >&2; exit 1
}
LLD=$(find_tool ld.lld "$ROOT/lld/usr/lib/llvm-18/bin/ld.lld")
NASM=$(find_tool nasm "$ROOT/nasm/usr/bin/nasm")
AR=$(find_tool llvm-ar llvm-ar-18 llvm-ar-19 llvm-ar-17)
JOBS=$(nproc 2>/dev/null || echo 4)

# The formats the player supports. Add decoders/demuxers here to extend it.
COMPONENTS="
--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
--disable-avdevice --disable-avfilter --disable-postproc --disable-debug --disable-iconv
--enable-static --disable-shared --enable-pthreads
--enable-protocol=file
--enable-demuxer=matroska,mov,avi,mpegts,mpegps,flv,asf,ogg,wav,mp3,aac,ac3,flac,m4v,h264,hevc,mpegvideo
--enable-decoder=h264,hevc,mpeg4,mpeg2video,mpeg1video,msmpeg4v2,msmpeg4v3,vp8,vp9,vc1,wmv3,mjpeg
--enable-decoder=aac,aac_latm,ac3,eac3,mp3,mp2,dca,truehd,flac,vorbis,opus,wmav2,pcm_s16le,pcm_s24le
--enable-decoder=subrip,srt,ass,ssa,mov_text,webvtt,text
--enable-decoder=pgssub,dvdsub,dvbsub
--enable-parser=h264,hevc,mpeg4video,mpegvideo,aac,aac_latm,ac3,mpegaudio,vp8,vp9,dca,flac,opus,vorbis,vc1,mjpeg,dvdsub,dvbsub
"

SRC="$ROOT/ffmpeg-src/ffmpeg-$FFMPEG_VERSION"
if [ ! -f "$SRC/configure" ]; then
    mkdir -p "$ROOT/ffmpeg-src"
    curl -L -o "$ROOT/ffmpeg-src/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    tar -xJf "$ROOT/ffmpeg-src/ffmpeg.tar.xz" -C "$ROOT/ffmpeg-src"
fi

# --- PS4 ---------------------------------------------------------------
CF="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -march=btver2 -isysroot $TC -isystem $TC/include"
LF="--target=x86_64-pc-freebsd12-elf -nostdlib --ld-path=$LLD -pie -Wl,-m,elf_x86_64 -Wl,--script,$TC/link.x -Wl,--eh-frame-hdr -L$TC/lib $TC/lib/crt1.o"

mkdir -p "$ROOT/ffmpeg-ps4-build"
cd "$ROOT/ffmpeg-ps4-build"
# shellcheck disable=SC2086
"$SRC/configure" --prefix="$ROOT/ffmpeg-ps4" --enable-cross-compile --arch=x86_64 --target-os=freebsd \
    --cc=clang --cxx=clang++ --ar="$AR" --ranlib=true --nm=nm --strip=true \
    --x86asmexe="$NASM" --pkg-config=false \
    --extra-cflags="$CF" --extra-ldflags="$LF" --extra-libs="-lc -lkernel" $COMPONENTS

# configure finds the sysctl() function in the PS4 libraries, but the
# toolchain has no <sys/sysctl.h>, so the build would fail without this.
sed -i 's/^#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' config.h

make -j"$JOBS"
make install

# --- PC (for `make host`) ------------------------------------------------
mkdir -p "$ROOT/ffmpeg-host-build"
cd "$ROOT/ffmpeg-host-build"
# shellcheck disable=SC2086
"$SRC/configure" --prefix="$ROOT/ffmpeg-host" --cc=clang --x86asmexe="$NASM" --pkg-config=false $COMPONENTS
make -j"$JOBS"
make install

echo "FFmpeg $FFMPEG_VERSION installed to $ROOT/ffmpeg-ps4 and $ROOT/ffmpeg-host"
