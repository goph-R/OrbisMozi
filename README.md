# Orbis Mozi

A video player for the PlayStation 4 that plays the files the built-in Media
Player refuses: MKV and other containers, decoded with FFmpeg.

It is homebrew for consoles that can run fake-signed packages (for example
with GoldHEN). *Mozi* is Hungarian for cinema; *Orbis* is the PS4's codename.

> **Status: early.** Playback works and stays in sync, but there are no
> controls yet. The player picks one file and plays it from start to end.

## What works

Tested on a PS4 Pro:

| Source | Video | Audio | Result |
|---|---|---|---|
| SD episode | H.264 720x404, 1.8 Mbit/s | AAC stereo | full episode in sync |
| 720p film | H.264 1280x720, 5.8 Mbit/s | AC3 5.1 | plays fine |
| 1080p film | H.264 1920x1038, 14 Mbit/s | AC3 5.1 | 24 fps, 0 dropped frames |

- Containers: MKV, MP4/MOV, AVI, MPEG-TS/PS, FLV, ASF/WMV, OGG.
- Video decoders built in: H.264, H.265, MPEG-4 (Xvid/DivX), MPEG-1/2, VP8,
  VP9, VC-1, WMV3, MJPEG. Only H.264 has been tried on the console so far.
- Audio decoders built in: AAC, AC3, E-AC3, MP3, MP2, DTS, TrueHD, FLAC,
  Vorbis, Opus, WMA, PCM. Multichannel audio is mixed down to stereo.
- The screen runs at 1280x720 for sources up to 720p and switches to
  1920x1080 for larger ones.
- Decoding is done on the CPU. 4K and heavy H.265 files are not expected to
  play smoothly.

## Not there yet

- Controller input: pause, seek, stop
- A file browser
- Choosing the audio track (the file's default track is used)
- Subtitles
- A way to hide the diagnostic line at the top of the picture

## Using it

1. Build the package (see below) and install it with your package installer.
2. Put a video into `/data/video` on the console's internal disk, for example
   with GoldHEN's FTP server (port 2121):

   ```sh
   curl -T film.mkv ftp://<ps4-ip>:2121/data/video/
   ```

   The player creates `/data/video` the first time it runs.
3. Start *Orbis Mozi*. It shows a log for a few seconds, then plays the
   alphabetically first video in `/data/video`. Subfolders are ignored.
4. Quit with the PS button.

**Why not a USB drive?** A PS4 application runs in a sandbox that does not
contain the USB mount points, so the player cannot see them. `/data` is
visible, which is why videos go there.

The line at the top of the picture shows the position, frames per second,
dropped frames, the time spent converting each frame, and the offset between
picture and sound. It is there for testing and will become optional.

## Building

Built and tested on Ubuntu 24.04 with clang 18.

### 1. Tools

`clang`, `lld`, `nasm`, `llvm-ar`, `make`, `curl`, and Python 3 with Pillow if
you want to regenerate the font or icon.

```sh
sudo apt install clang lld nasm llvm make curl
```

### 2. The OpenOrbis toolchain

Everything is expected in one folder next to this repository:

```
PS4-Toolchain/
  OpenOrbis/PS4Toolchain/     OpenOrbis release (headers, libraries, tools)
  ffmpeg-ps4/                 FFmpeg for the PS4   (built in step 3)
  ffmpeg-host/                FFmpeg for the PC    (built in step 3)
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

### 3. FFmpeg

```sh
tools/build-ffmpeg.sh
```

This downloads FFmpeg 7.1.1 and builds static libraries twice: for the PS4
(`ffmpeg-ps4`) and for the PC (`ffmpeg-host`, used only for testing). The list
of enabled formats is at the top of the script.

### 4. The package

```sh
make
```

The result is `IV0000-MOZI00001_00-ORBISMOZI0000000.pkg`.

Three system files every package needs (`right.sprx`, `libc.prx`,
`libSceFios2.prx`) are copied from the toolchain's samples during the build;
they are not part of this repository.

Any video placed in `assets/videos/` is packed into the package and played
when `/data/video` has none. That gives a self-contained test package.

## Testing on the PC

The player core does not depend on the console. `make host` builds it against
a fake display and a fake audio device that consume frames and samples in real
time, so decoding, timing and sync can be checked without a PS4:

```sh
make host
build/player_host film.mkv 30        # play the first 30 seconds
```

It prints the same log as the console build, ending with a summary line
(frames shown, frames dropped, worst picture/sound offset). Useful variables:

| Variable | Effect |
|---|---|
| `PLAYER_DUMP_DIR=dir` | writes every 240th displayed frame there as a PPM image |
| `PLAYER_WAV=file.wav` | writes the audio that would have been played |
| `PLAYER_SIZE=1920x1080` | size of the fake screen at start |

## How it works

| File | Role |
|---|---|
| `src/player.c` | the player: demuxing, decoding, sync |
| `src/plat.h` | the small interface the player needs from the machine |
| `src/plat_ps4.c` | PS4 implementation: `sceVideoOut` frame buffers, `sceAudioOut` |
| `host/plat_host.c` | PC implementation used by `make host` |
| `src/osd.c` | on-screen text and log (errors are otherwise invisible on a console) |
| `src/main.c` | start-up: finds a file, runs the player, shows the result |

Four threads: one reads packets from the file, one decodes video, one decodes
audio and feeds the audio device, and the main thread shows each frame when the
audio clock reaches its timestamp. Audio is the master clock, so picture and
sound cannot drift apart. Frames that arrive more than 100 ms late are dropped
so the picture can catch up.

Frames are converted from YUV to RGB (and scaled, if needed) by libswscale
directly into the frame buffer that is not on screen, then flipped at the next
vertical sync.

## Licence

Orbis Mozi is released under the [MIT licence](LICENSE).

It builds on other people's work, which keeps its own licences:

- **FFmpeg** is linked statically and is licensed under the LGPL 2.1 or later
  (the build script enables no GPL or non-free parts). If you distribute a
  built package, the LGPL's terms apply to the FFmpeg part of it; the build
  script and the sources here are what lets others rebuild it.
- **OpenOrbis PS4 Toolchain** (GPL 3.0) is used to build; its system files are
  copied at build time and are not included here.
- **DejaVu Sans Mono** is the source of the on-screen font in
  `src/font_data.h` (generated by `tools/mkfont.py`).

This project is not affiliated with or endorsed by Sony Interactive
Entertainment. "PlayStation" and "PS4" are trademarks of their owner. Use it
with files you have the right to play.
