/* PC back end for testing: no window, no sound card.
 * - "Display": two memory buffers, flips paced at 60 Hz.
 * - "Audio device": consumes samples in real time like a sound card would.
 * Environment:
 *   PLAYER_DUMP_DIR   write every 240th shown frame there as PPM
 *   PLAYER_WAV        write all audio to this WAV file
 *   PLAYER_SIZE       WxH of the fake screen (default 1280x720)
 *   PLAYER_KEYS       scripted controller, e.g. "d d x 5000 o": u d l r = d-pad,
 *                     x o t s = buttons, p = OPTIONS, L R = L1/R1, a number = wait that many milliseconds.
 *                     Each press takes 300 ms. The program exits when the script ends.
 *   PLAYER_DUMP_EVERY write every Nth frame instead of every 240th */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/plat.h"

static int       g_w = 1280, g_h = 720;
static uint32_t *g_buf[2];
static int       g_back;
static uint64_t  g_next_vsync;
static long      g_flips;
static FILE     *g_wav;
static uint64_t  g_audio_start;
static uint64_t  g_audio_frames;

uint64_t plat_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

void plat_sleep_us(unsigned us) { usleep(us); }

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t v32; uint16_t v16;
    fwrite("RIFF", 4, 1, f); v32 = 36 + frames * 4; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 8, 1, f); v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f); v16 = 2; fwrite(&v16, 2, 1, f);
    v32 = AUDIO_RATE; fwrite(&v32, 4, 1, f); v32 = AUDIO_RATE * 4; fwrite(&v32, 4, 1, f);
    v16 = 4; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 4, 1, f); v32 = frames * 4; fwrite(&v32, 4, 1, f);
}

static void finish(void)
{
    if (g_wav) {
        rewind(g_wav);
        wav_header(g_wav, (uint32_t)g_audio_frames);
        fclose(g_wav);
    }
}

int plat_init(void)
{
    const char *size = getenv("PLAYER_SIZE");
    if (size)
        sscanf(size, "%dx%d", &g_w, &g_h);
    for (int i = 0; i < 2; i++)
        g_buf[i] = calloc((size_t)g_w * g_h, 4);

    if (getenv("PLAYER_WAV")) {
        g_wav = fopen(getenv("PLAYER_WAV"), "wb");
        if (g_wav)
            wav_header(g_wav, 0);
    }
    atexit(finish);
    g_next_vsync = plat_time_us();
    return 0;
}

int plat_set_mode(int w, int h)
{
    if (w == g_w && h == g_h)
        return 0;
    for (int i = 0; i < 2; i++) {
        free(g_buf[i]);
        g_buf[i] = calloc((size_t)w * h, 4);
    }
    g_w = w;
    g_h = h;
    return 0;
}

int plat_width(void)  { return g_w; }
int plat_height(void) { return g_h; }
uint32_t *plat_backbuffer(void) { return g_buf[g_back]; }

void plat_flip(void)
{
    const char *dir = getenv("PLAYER_DUMP_DIR");

    int every = getenv("PLAYER_DUMP_EVERY") ? atoi(getenv("PLAYER_DUMP_EVERY")) : 240;
    if (every < 1)
        every = 1;

    if (dir && g_flips % every == every / 2) {
        char name[512];
        snprintf(name, sizeof(name), "%s/frame_%05ld.ppm", dir, g_flips);
        FILE *f = fopen(name, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
            for (int i = 0; i < g_w * g_h; i++) {
                uint32_t p = g_buf[g_back][i];
                unsigned char rgb[3] = { p >> 16, p >> 8, p };
                fwrite(rgb, 3, 1, f);
            }
            fclose(f);
        }
    }
    g_flips++;

    /* Wait for the next 60 Hz tick. */
    uint64_t now = plat_time_us();
    while (g_next_vsync <= now)
        g_next_vsync += 16667;
    usleep((useconds_t)(g_next_vsync - now));
    g_back ^= 1;
}

/* Plays back the PLAYER_KEYS script against the clock. */
unsigned plat_pad(void)
{
    static uint64_t start;
    const char *script = getenv("PLAYER_KEYS");
    uint64_t now = plat_time_us(), t = 0;

    if (!script)
        return 0;
    if (!start)
        start = now;
    now = (now - start) / 1000;                     /* ms since the first call */

    for (const char *p = script; *p; ) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (*p >= '0' && *p <= '9') {
            t += strtoul(p, (char **)&p, 10);
            if (now < t)
                return 0;
            continue;
        }
        unsigned button = 0;
        switch (*p++) {
        case 'u': button = PAD_UP; break;
        case 'd': button = PAD_DOWN; break;
        case 'l': button = PAD_LEFT; break;
        case 'r': button = PAD_RIGHT; break;
        case 'x': button = PAD_CROSS; break;
        case 'o': button = PAD_CIRCLE; break;
        case 't': button = PAD_TRIANGLE; break;
        case 's': button = PAD_SQUARE; break;
        case 'p': button = PAD_OPTIONS; break;
        case 'L': button = PAD_L1; break;
        case 'R': button = PAD_R1; break;
        }
        if (now < t + 100)
            return button;
        t += 300;
        if (now < t)
            return 0;
    }
    exit(0);                                        /* script finished */
}

int plat_keyboard(const char *title, char *buf, int buf_size, int password, void (*redraw)(void))
{
    (void)title; (void)buf; (void)buf_size; (void)password; (void)redraw;
    return -1;                                      /* the built-in keyboard gets tested instead */
}

void plat_audio_write(const int16_t *pcm)
{
    if (g_wav)
        fwrite(pcm, 4, AUDIO_GRAIN, g_wav);

    /* Behave like a device with a two-grain buffer playing in real time. */
    uint64_t now = plat_time_us();
    if (g_audio_frames == 0)
        g_audio_start = now;
    g_audio_frames += AUDIO_GRAIN;

    uint64_t played_until = g_audio_start + g_audio_frames * 1000000u / AUDIO_RATE;
    if (played_until < now) {                       /* ran dry (pause): a device does not catch up */
        g_audio_start += now - played_until;
        played_until = now;
    }
    uint64_t slack = 2u * AUDIO_GRAIN * 1000000u / AUDIO_RATE;
    if (played_until > now + slack)
        usleep((useconds_t)(played_until - now - slack));
}
