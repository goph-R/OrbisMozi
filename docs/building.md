# Building Orbis Mozi

How to build the package yourself, and how to test the player on a PC. To just
use the player, download the package from the
[releases](https://github.com/goph-R/OrbisMozi/releases) instead.

Built and tested on Ubuntu 24.04 with clang 18.

## 1. Tools

`clang`, `lld`, `nasm`, `llvm-ar`, `make`, `cmake`, `git`, `curl`, and Python 3 with Pillow if
you want to regenerate the fonts.

```sh
sudo apt install clang lld nasm llvm make curl cmake git
```

## 2. The OpenOrbis toolchain

Everything is expected in one folder next to this repository:

```
PS4-Toolchain/
  OpenOrbis/PS4Toolchain/     OpenOrbis release (headers, libraries, tools)
  ffmpeg-ps4/                 FFmpeg for the PS4   (built in step 3)
  ffmpeg-host/                FFmpeg for the PC    (built in step 3)
  libsmb2-ps4/  libsmb2-host/ libsmb2              (built in step 4)
OrbisMozi/                    this repository
```

```sh
mkdir -p ../PS4-Toolchain && cd ../PS4-Toolchain
curl -L -o oo.tar.gz \
  https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
tar -xzf oo.tar.gz && rm oo.tar.gz
```

A different location works too: pass `PS4TC=/path/to/folder` to `make` and as
the argument of the FFmpeg script.

**libssl 1.1.** The toolchain's packaging tool (`PkgTool.Core`) needs OpenSSL
1.1 and aborts with `No usable version of libssl was found` on current
distributions. Unpack Ubuntu 20.04's `libssl1.1` package into
`PS4-Toolchain/libssl11/`; the Makefile adds it to the library path for that
one tool:

```sh
mkdir -p libssl11 && cd libssl11
# take the newest libssl1.1_1.1.1f-1ubuntu2.NN_amd64.deb from
#   http://security.ubuntu.com/ubuntu/pool/main/o/openssl/
dpkg-deb -x libssl1.1_*.deb .
```

## 3. FFmpeg

```sh
tools/build-ffmpeg.sh
```

This downloads FFmpeg 7.1.1 and builds static libraries twice: for the PS4
(`ffmpeg-ps4`) and for the PC (`ffmpeg-host`, used only for testing). The list
of enabled formats is at the top of the script.

## 4. libsmb2

```sh
tools/build-libsmb2.sh
```

This fetches libsmb2 7.0.0 (the SMB2/SMB3 client used for network shares) and
builds it for the PS4 and the PC. It needs `cmake` and `git`.

## 5. The package

```sh
make
```

The result is `IV0000-MOZI00001_00-ORBISMOZI0000000.pkg`.

Three system files every package needs (`right.sprx`, `libc.prx`,
`libSceFios2.prx`) are copied from the toolchain's samples during the build;
they are not part of this repository.

## Testing on the PC

The player core does not depend on the console. `make host` builds it against
a fake display and a fake audio device that consume frames and samples in real
time, so decoding, timing and sync can be checked without a PS4:

```sh
make host
build/player_host film.mkv 30        # play the first 30 seconds
build/player_host smb://user:password@host/share/film.mkv 30
```

Without arguments it shows the file list, driven by a scripted controller.

It prints the same log as the console build, ending with a summary line
(frames shown, frames dropped, worst picture/sound offset). Useful variables:

| Variable | Effect |
|---|---|
| `PLAYER_DUMP_DIR=dir` | writes every 240th displayed frame there as a PPM image |
| `PLAYER_WAV=file.wav` | writes the audio that would have been played |
| `PLAYER_DUMP_EVERY=n` | write every nth frame instead |
| `PLAYER_SIZE=1920x1080` | size of the fake screen at start |
| `PLAYER_KEYS="d x 5000 o"` | scripted button presses: `u d l r` d-pad, `x o t` buttons, `L R` L1/R1, a number waits that many ms |
| `MOZI_LOCAL=dir` | folder shown as the console's video folder |
| `MOZI_SHARES=file` | shares file to read instead of `/data/orbismozi/shares.txt` |
