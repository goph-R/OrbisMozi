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

uint64_t  plat_time_us(void);
void      plat_sleep_us(unsigned us);

#endif
