# Roadmap

## Not there yet

- Picture-based subtitles (Blu-ray PGS, DVD) and separate `.srt` files
- Remembering the preferred audio and subtitle language
- A designed playback overlay (the menu, the file list and the share form
  are designed; the overlay is still plain text)

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
