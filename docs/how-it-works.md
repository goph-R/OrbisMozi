# How it works

| File | Role |
|---|---|
| `src/player.c` | the player: demuxing, decoding, sync |
| `src/plat.h` | the small interface the player needs from the machine |
| `src/plat_ps4.c` | PS4 implementation: `sceVideoOut` frame buffers, `sceAudioOut` |
| `host/plat_host.c` | PC implementation used by `make host` |
| `src/osd.c` | on-screen text and log (errors are otherwise invisible on a console) |
| `src/subfont.c` | the subtitle font: anti-aliased, accented letters, outlined |
| `src/ui.c` | the menu's font, pictures and shapes (`src/uifont.bin`, `src/uiart.bin`; the pictures come from `assets/ui/` via `tools/mkuiart.py`) |
| `src/smb.c` | network shares: folder listing and file reading through libsmb2 |
| `src/browser.c` | the file list |
| `src/shareform.c` | the form for adding or changing a network share |
| `src/textinput.c` | text entry: the system keyboard, or a built-in one |
| `src/main.c` | start-up and the loop: pick a file, play it, repeat |

The mockups the menu screens are built from are in `design/` (layered PSD
files at 1920x1080, drawn in PhotoCraft), together with the icon's source
and the menu background as PNG, for use in mockups.

Four threads: one reads packets from the file, one decodes video, one decodes
audio and feeds the audio device, and the main thread shows each frame when the
audio clock reaches its timestamp. Audio is the master clock, so picture and
sound cannot drift apart. Frames that arrive more than 100 ms late are dropped
so the picture can catch up.

Frames are converted from YUV to RGB (and scaled, if needed) by libswscale
directly into the frame buffer that is not on screen, then flipped at the next
vertical sync.

## Diagnostics

The diagnostics line (Triangle while playing) shows the position, frames per
second, dropped frames, the time spent converting each frame, and the offset
between picture and sound.

The app writes its log to `/data/orbismozi/log.txt` on every start.

## Files on the console

On its first start the app creates two folders on the console's disk:
`/data/video` for videos and `/data/orbismozi` for its settings.

Network shares are kept in `/data/orbismozi/shares.txt`, one
`smb://user:password@server/share` per line, with the password as plain text.
The file can also be edited by hand.

The video played last and its position are kept in
`/data/orbismozi/resume.txt` (the position in seconds on the first line, the
location without a share's password on the second). Only this one video is
remembered; deleting the file forgets it.

## Why not a USB drive?

A PS4 application runs in a sandbox that does not contain the USB mount
points, so the player cannot see them. `/data` is visible, which is why videos
go there.
