# Changelog

All notable changes to Orbis Mozi are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the version
numbers follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.0] - 2026-11-21

First report from a base PS4: a 1080p H.264 film from a Samba share plays
without dropped frames.

### Added

- A subtitle file beside the video (the same name with `.srt`) is loaded and
  switched on. UTF-8, UTF-16 and Windows-1250 text are read.
- The video played last continues where it was left when it is started
  again; one that was seen to the end starts from the beginning.
- While playing, any button shows an overlay with the position, the subtitle
  and the audio track in use.
- Circle pauses and opens a menu: continue, choose subtitles, choose audio,
  picture, rewind, help and exit.
- Picture subtitles inside the file (Blu-ray PGS, DVD, DVB) are listed and
  shown.
- Picture modes: fit (the whole picture), fill (no borders, edges cut off)
  and stretch.
- The share form in its designed look, with a question before a share is
  deleted.

## [0.1.0] - 2026-11-21

The first public release.

### Added

- Video playback with FFmpeg 7.1, decoded on the CPU, with the sound as the
  master clock so picture and sound stay in sync.
- Video: H.264 and H.265 (8 and 10 bit), MPEG-4 (Xvid, DivX), MPEG-2, VP8
  and VP9. Audio: AAC, AC3, E-AC3, DTS, FLAC, MP3, MP2, Opus and Vorbis.
  Containers: MKV, MP4, AVI, FLV, WebM, MPEG-PS and MPEG-TS.
- Screen mode follows the video: 1280x720 for sources up to 720p, 1920x1080
  for larger ones.
- Playing from the console's disk (`/data/video`) and from network shares
  (SMB2/SMB3), with a form to add, test, change and delete shares.
- Text entry with the system keyboard, and a built-in keyboard as fallback.
- Main menu and file list with the Noto Sans font, icons, button hints and an
  animated background.
- Pause, stop, jumps of 10 seconds and 10 minutes, a position bar, and volume
  from 0 to 200%.
- Choice of audio track, and text subtitles (SRT, ASS/SSA as plain text) with
  accented letters.
- A diagnostics line (frames per second, dropped frames, sync offset) and a
  log file at `/data/orbismozi/log.txt`.
- A PC test build (`make host`) that runs the player without a console.

### Known limitations

- Tested on a PS4 Pro only.
- 4K and heavy H.265 files are not expected to play smoothly.
- No picture-based subtitles (Blu-ray PGS, DVD) and no separate `.srt` files.
- Multichannel audio is mixed down to stereo.
- USB drives cannot be read; an application's sandbox does not contain them.

[1.0.0]: https://github.com/goph-R/OrbisMozi/releases/tag/v1.0.0
[0.1.0]: https://github.com/goph-R/OrbisMozi/releases/tag/v0.1.0
