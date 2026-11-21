# What plays

Everything here was tried on a PS4 Pro. Decoding is done on the CPU, so the
limits depend on the console. On a base PS4 one report exists so far: a 1080p
H.264 film from a Samba share played with no dropped frames (2.8 ms per frame
for conversion, picture and sound 4 ms apart).

## Tested formats

All of these played, each from a short test clip:

| Video | Tested as | Audio in the clip | Container |
|---|---|---|---|
| H.264 | 720p, 720p 60 fps, 1080p 18 Mbit/s | AAC, AC3 5.1 | MKV, MP4, FLV |
| H.264 10-bit | 720p | AAC | MKV |
| H.265 (HEVC) | 720p, 1080p | AAC | MKV |
| H.265 10-bit | 1080p | AAC | MKV |
| MPEG-4 (Xvid) | 624x352 | MP3 | AVI |
| MPEG-4 (DivX 3) | 624x352 | MP3 | AVI |
| MPEG-4 | 720p | AC3 | MKV |
| MPEG-2 | DVD (PAL), 720p | AC3, MP2 | MPEG-PS, MPEG-TS |
| VP9 | 720p, 1080p | Opus | WebM |
| VP8 | 720p | Vorbis | WebM |

| Audio | Tested as |
|---|---|
| AAC | stereo (48 kHz and 44.1 kHz) |
| AC3 | stereo and 5.1 |
| E-AC3 | 5.1 |
| DTS | 5.1 |
| FLAC | 5.1 |
| MP3, MP2, Opus, Vorbis | stereo |

| Subtitles | Tested as |
|---|---|
| SubRip (SRT) | two tracks in one MKV, with accented letters |
| ASS/SSA | one track in the same MKV (text only, styling is ignored) |

Also built in but not tried on the console yet: VC-1, WMV3, MJPEG and
MPEG-1 video; TrueHD, WMA and PCM audio; MP4 timed text and WebVTT
subtitles; the ASF/WMV and OGG containers.

Multichannel audio is mixed down to stereo. Text subtitles are shown as plain
text; styling and positioning are ignored.

## Whole files

| Source | Video | Audio | Result |
|---|---|---|---|
| SD episode | H.264 720x404, 1.8 Mbit/s | AAC stereo | full episode in sync |
| 720p film | H.264 1280x720, 5.8 Mbit/s | AC3 5.1 | plays fine |
| 1080p film | H.264 1920x1038, 14 Mbit/s | AC3 5.1 | 24 fps, 0 dropped frames |

A 1080p film from a network share (Samba) starts in well under a second.

## Limits

- 4K and heavy H.265 files are not expected to play smoothly.
- Picture-based subtitles (Blu-ray PGS, DVD) are not supported yet.
- A subtitle file beside the video is used when it has the video's exact name
  with the `.srt` extension (`Film.mkv` and `Film.srt`). It is switched on by
  itself and comes first in the list, before the tracks inside the video.
  UTF-8 and UTF-16 are recognised; anything else is read as Windows-1250
  (Central European). Other subtitle file types (`.ass`, `.vtt`) are not read.
- The screen runs at 1280x720 for sources up to 720p and switches to
  1920x1080 for larger ones.
- Jumps land on the nearest key frame, so the distance is approximate.
  Changing the audio track also jumps back to the last key frame, a few
  seconds at most.
