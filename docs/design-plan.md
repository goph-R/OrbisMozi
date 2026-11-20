# Orbis Mozi – design plan

The interface is plain text at the moment. This plan is ordered so that each
step gives a visible result, and nothing has to be designed in full before
something can be seen on the TV.

## 1. Decisions first (no drawing yet)

| Decision | Recommendation |
|---|---|
| Canvas size | 1920x1080, with a safe margin of 96 px left/right and 54 px top/bottom |
| List or tiles | A list. Tiles need thumbnails, and the player has none |
| UI font | **Decided: Noto Sans**, in several sizes (SIL Open Font License, has the Hungarian accents ő, ű). Still open: which weights and which pixel sizes |
| Line width | **Decided: 2 px** in the 1080p mockup (drawn as a crisp 1 px line in the 720p menu). Small gaps, paddings and radii in multiples of 3 |
| Palette | 5–6 colours: background, panel, text, dimmed text, accent (selection), error |
| Button hints | Own drawn ✕ ○ □ △ shapes rather than text like "X open" |

## 2. Screens, in build order

### Step A – the list (used most, biggest effect)

- Top-level list: "This console", the network shares, "Add network share..."
- Folder view: path at the top, folders and videos, selected row, position
  counter ("12 / 48"), button hints at the bottom
- States to draw: normal row, selected row, folder vs video, empty folder,
  "Connecting...", error message
- Decide what a row shows: only the name, or also a type icon and file size

### Step B – playback overlay

- Progress bar with time and total length
- Notes for volume, audio track and subtitle track (plain text boxes today)
- A pause indicator (there is none today)
- Optional: one combined panel with title, progress and current tracks

### Step C – the share form

- Five fields (server, share, user, password, folder), three actions (test,
  save, delete), and the result line of the connection test in a success and
  an error version
- The built-in keyboard is only a fallback when the system keyboard is not
  available; it can simply inherit the colours and font

### Step D – outside the app

- App icon, 512x512 (`sce_sys/icon0.png`; the current one is a placeholder)
- A background picture for the PS4 home screen, 1920x1080. To be verified
  that packages built with this toolchain can carry one before it is drawn.

## 3. What to deliver per step

- One full-screen PNG mockup per screen or state, at 1920x1080
- Separate elements as PNG with transparency, or SVG: icons, button symbols,
  logo
- The font files
- Colours as hex values, and the sizes that matter: row height, text sizes,
  corner radius, margins

## 4. Constraints

- Everything is drawn by the CPU. Flat fills, gradients, rounded corners,
  semi-transparent panels and icons are fine. Blur and large soft drop
  shadows are not.
- The playback overlay has to work at 1280x720 and at 1920x1080 (the screen
  switches with the video). Use even pixel sizes, so everything lands on
  whole pixels when scaled by 1.5.
- Keep animation minimal. A moving selection or a fading bar is fine; sliding
  whole screens is not worth the work.
- Text has to be readable from the sofa: smallest text 28 px or more on the
  1080p canvas.
- L2 and R2 are not used for anything.

## 5. How the work goes

1. Step A as rough mockups, even unfinished.
2. It gets built and looked at on the TV. Colours and sizes look different
   there than on a monitor.
3. Adjust, then repeat for B, C and D.

The first things needed are only the **font** and the **palette**. With those
two the current screens can already be restyled, and the rest can be designed
against something real.

## Current controls (for reference)

| Button | In the list | While playing |
|---|---|---|
| D-pad up/down | move | volume up / down |
| D-pad left/right | page up / down | 10 seconds back / forward |
| L1 / R1 | page up / down | 10 minutes back / forward |
| Cross | open folder / play | pause, continue |
| Circle | back | stop, return to the list |
| Square | change or delete a share | next audio track |
| OPTIONS | | next subtitle track, then off |
| Triangle | | show / hide the diagnostics line |

The mapping is provisional and will be revised together with the design.
