# Orbis Mozi

A video player for the PlayStation 4 that plays the files the built-in Media
Player refuses: MKV and other containers, decoded with FFmpeg.

It is homebrew for consoles that can run fake-signed packages (for example
with GoldHEN). *Mozi* is Hungarian for cinema; *Orbis* is the PS4's codename.

> **Status: early.** Playback works and stays in sync. There is a plain file
> list, pause, stop, seeking, audio track choice and text subtitles.

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
- Subtitle decoders built in: SubRip (SRT), ASS/SSA, MP4 timed text, WebVTT.
- Audio decoders built in: AAC, AC3, E-AC3, MP3, MP2, DTS, TrueHD, FLAC,
  Vorbis, Opus, WMA, PCM. Multichannel audio is mixed down to stereo.
- The screen runs at 1280x720 for sources up to 720p and switches to
  1920x1080 for larger ones.
- Files can be played from the console's disk or straight from a network
  share (SMB2/SMB3). Tested with a Samba server: a 1080p film starts in
  well under a second.
- Decoding is done on the CPU. 4K and heavy H.265 files are not expected to
  play smoothly.

## Not there yet

- Picture-based subtitles (Blu-ray PGS, DVD) and separate `.srt` files
- Remembering the preferred audio and subtitle language
- A designed interface (the list is plain text for now)

## Future improvements

Ideas that are not being worked on yet.

### Audio player

Playing music files as well as videos. The decoders are already built in
(MP3, FLAC, AAC/M4A, Ogg Vorbis, Opus, WAV, WMA), and audio output, volume,
pause, seeking and network shares would work unchanged. What is missing:

- An audio-only playback path: the player's main loop is driven by video
  frames and refuses a file without video.
- A "now playing" screen: title, artist and album from the file's tags, a
  progress bar, and the embedded cover picture if there is one.
- A playlist: next, previous and random within a folder, moving on by itself
  when a track ends. The same mechanism would give "play the next episode"
  for series.
- Audio file types in the list, with their own icon.

Not part of the idea: playing on while another application is in front (an
application is suspended when it is left), and gapless playback between
tracks.

## Using it

1. Build the package (see below) and install it with your package installer.
2. Start *Orbis Mozi* once. It creates two folders on the console's disk:
   `/data/video` for videos and `/data/orbismozi` for its settings.
3. Give it something to play, either or both:
   - **Copy videos to the console**, for example with GoldHEN's FTP server
     (port 2121). Subfolders are fine.

     ```sh
     curl -T film.mkv ftp://<ps4-ip>:2121/data/video/
     ```
   - **Add a network share.** Choose "Add network share..." in the list
     and fill in the server address, share name, user and password. "Test
     connection" tells you whether it works before you save. Square on a
     share in the list changes or deletes it.

     The server has to offer SMB2 or SMB3; the old SMB1 protocol alone is
     not enough. The password is stored on the console as plain text, in
     `/data/orbismozi/shares.txt` (one `smb://user:password@server/share`
     per line, which can also be edited by hand).
4. Pick a video in the list.

The app writes its log to `/data/orbismozi/log.txt` on every start, which is
the thing to look at (or attach to a bug report) when something does not work.

| Button | In the list | While playing |
|---|---|---|
| D-pad up/down | move | volume up / down (0–200%) |
| D-pad left/right | page up / down | 10 seconds back / forward (hold to keep going) |
| L1 / R1 | page up / down | 10 minutes back / forward |
| Cross | open folder / play | pause, continue |
| Square | change or delete a share | next audio track |
| OPTIONS | | next subtitle track, then off |
| Circle | back | stop, return to the list |
| Triangle | | show / hide the diagnostics line |
| PS | | quit |

Jumps land on the nearest key frame, so the distance is approximate. Changing
the audio track also jumps back to the last key frame, a few seconds at most.

Subtitles: text tracks inside the file (SRT, ASS/SSA, MP4 timed text,
WebVTT) are shown as plain text; styling and positioning are ignored.

**Why not a USB drive?** A PS4 application runs in a sandbox that does not
contain the USB mount points, so the player cannot see them. `/data` is
visible, which is why videos go there.

The diagnostics line (triangle) shows the position, frames per second,
dropped frames, the time spent converting each frame, and the offset between
picture and sound.

## Building

Built and tested on Ubuntu 24.04 with clang 18.

### 1. Tools

`clang`, `lld`, `nasm`, `llvm-ar`, `make`, `cmake`, `git`, `curl`, and Python 3 with Pillow if
you want to regenerate the font or icon.

```sh
sudo apt install clang lld nasm llvm make curl cmake git
```

### 2. The OpenOrbis toolchain

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

### 3. FFmpeg

```sh
tools/build-ffmpeg.sh
```

This downloads FFmpeg 7.1.1 and builds static libraries twice: for the PS4
(`ffmpeg-ps4`) and for the PC (`ffmpeg-host`, used only for testing). The list
of enabled formats is at the top of the script.

### 4. libsmb2

```sh
tools/build-libsmb2.sh
```

This fetches libsmb2 7.0.0 (the SMB2/SMB3 client used for network shares) and
builds it for the PS4 and the PC. It needs `cmake` and `git`.

### 5. The package

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

## How it works

| File | Role |
|---|---|
| `src/player.c` | the player: demuxing, decoding, sync |
| `src/plat.h` | the small interface the player needs from the machine |
| `src/plat_ps4.c` | PS4 implementation: `sceVideoOut` frame buffers, `sceAudioOut` |
| `host/plat_host.c` | PC implementation used by `make host` |
| `src/osd.c` | on-screen text and log (errors are otherwise invisible on a console) |
| `src/subfont.c` | the subtitle font: anti-aliased, accented letters, outlined |
| `src/smb.c` | network shares: folder listing and file reading through libsmb2 |
| `src/browser.c` | the file list |
| `src/shareform.c` | the form for adding or changing a network share |
| `src/textinput.c` | text entry: the system keyboard, or a built-in one |
| `src/main.c` | start-up and the loop: pick a file, play it, repeat |

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
- **libsmb2** is linked statically too and is also LGPL 2.1 or later; the
  same applies to it.
- **OpenOrbis PS4 Toolchain** (GPL 3.0) is used to build; its system files are
  copied at build time and are not included here.
- **DejaVu Sans Mono** is the source of the on-screen font in
  `src/font_data.h` (generated by `tools/mkfont.py`), and **DejaVu Sans
  Bold** of the subtitle font in `src/subfont.bin` (`tools/mksubfont.py`).

This project is not affiliated with or endorsed by Sony Interactive
Entertainment. "PlayStation" and "PS4" are trademarks of their owner. Use it
with files you have the right to play.
