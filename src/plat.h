/* Platform interface: everything the player needs from the machine.
 * Implemented by src/plat_ps4.c (console) and host/plat_host.c (PC test). */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>

#define AUDIO_RATE   48000
#define AUDIO_GRAIN  256          /* stereo frames per plat_audio_write() */

/* Opens video and audio. Returns 0 on success. */
int       plat_init(void);

/* Switches the screen buffers to w x h (1280x720 or 1920x1080).
 * Returns 0 if the screen now has that size; on failure the old size stays. */
int       plat_set_mode(int w, int h);

int       plat_width(void);
int       plat_height(void);

/* The buffer that is not on screen: width*height pixels, 0xAARRGGBB.
 * Treat it as write-only. */
uint32_t *plat_backbuffer(void);

/* Shows the back buffer at the next vertical sync and waits for it. */
void      plat_flip(void);

/* Queues AUDIO_GRAIN stereo S16 frames; blocks until there is room. */
void      plat_audio_write(const int16_t *pcm);

/* Controller buttons held down right now, as a mask of PAD_* bits. */
#define PAD_UP        0x0001u
#define PAD_DOWN      0x0002u
#define PAD_LEFT      0x0004u
#define PAD_RIGHT     0x0008u
#define PAD_CROSS     0x0010u
#define PAD_CIRCLE    0x0020u
#define PAD_TRIANGLE  0x0040u
#define PAD_SQUARE    0x0080u
#define PAD_L1        0x0100u
#define PAD_R1        0x0200u
#define PAD_OPTIONS   0x0400u
unsigned  plat_pad(void);

/* The system's on-screen keyboard, editing the UTF-8 text in buf.
 * Returns 1 if confirmed, 0 if cancelled, -1 if there is no such keyboard
 * (the caller then uses its own). redraw() is called once per frame and
 * must draw and flip. */
int       plat_keyboard(const char *title, char *buf, int buf_size, int password, void (*redraw)(void));

uint64_t  plat_time_us(void);
void      plat_sleep_us(unsigned us);

#endif
