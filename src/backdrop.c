/* Everything here is drawn by the CPU. The still part (colour gradient and
 * soft glows) is painted once and copied every frame; only the dust is
 * computed per frame, and that is a few thousand pixels. Blurry dust is not
 * blurred: each speck is drawn directly with a soft edge of the wanted width. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "backdrop.h"
#include "plat.h"

#define SPECKS 110
#define DIM    0.375f               /* brightness of a shaded panel */

typedef struct {
    float x, y;             /* position in pixels of a 720-line screen */
    float blur;             /* 0 = small and sharp (near), 1 = large and soft (out of focus) */
    float speed;            /* pixels per second, upwards */
    float sway, sway_rate;  /* sideways drift */
    float phase, twinkle;
} Speck;

static uint32_t *g_still;           /* the painted background */
static uint32_t *g_dim;             /* the same, darkened, for behind text */
static int       g_w, g_h;
static Speck     g_specks[SPECKS];
static int       g_seeded;
static uint64_t  g_last;
static uint32_t  g_rng = 0x9E3779B9u;

static float frand(void)            /* 0..1 */
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFF) / 16777216.0f;
}

static float glow(float x, float y, float cx, float cy, float radius)
{
    float dx = (x - cx) / radius, dy = (y - cy) / radius;
    float d2 = dx * dx + dy * dy;
    if (d2 >= 1.0f)
        return 0.0f;
    float f = 1.0f - d2;
    return f * f;
}

/* Paints the still picture for a w x h screen. Coordinates are in units of
 * the screen height, so it looks the same at 720 and 1080 lines. */
static void paint_still(int w, int h)
{
    free(g_still);
    free(g_dim);
    g_still = malloc((size_t)w * h * 4);
    g_dim = malloc((size_t)w * h * 4);
    g_w = w;
    g_h = h;

    float aspect = (float)w / h;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            float x = (float)px / h, y = (float)py / h;
            float t = (x / aspect) * 0.45f + y * 0.55f;                /* diagonal: light top left, dark bottom right */

            float r = 10.0f + (3.0f - 10.0f) * t;
            float g = 52.0f + (14.0f - 52.0f) * t;
            float b = 128.0f + (48.0f - 128.0f) * t;

            float a = glow(x, y, aspect * 0.78f, 0.30f, 0.85f) * 0.55f;   /* bright cloud, upper right */
            r += (40.0f - r) * a;  g += (120.0f - g) * a;  b += (215.0f - b) * a;
            a = glow(x, y, aspect * 0.18f, 0.95f, 0.70f) * 0.40f;         /* cooler one, lower left */
            r += (20.0f - r) * a;  g += (80.0f - g) * a;   b += (180.0f - b) * a;
            a = glow(x, y, aspect * 0.50f, 0.55f, 0.30f) * 0.12f;         /* faint centre lift */
            r += (90.0f - r) * a;  g += (160.0f - g) * a;  b += (235.0f - b) * a;

            /* A little noise hides the steps a smooth gradient would show on a TV. */
            float n = frand() - 0.5f;
            int ir = (int)(r + n + 0.5f), ig = (int)(g + n + 0.5f), ib = (int)(b + n + 0.5f);
            if (ir < 0) ir = 0;
            if (ig < 0) ig = 0;
            if (ib < 0) ib = 0;
            g_still[(size_t)py * w + px] = 0xFF000000u | (uint32_t)ir << 16 | (uint32_t)ig << 8 | (uint32_t)ib;

            /* The dark version is computed from the exact colour, not from
             * the rounded one, or its few brightness steps would show as stripes. */
            n = frand() - 0.5f;
            ir = (int)(r * DIM + n + 0.5f);  ig = (int)(g * DIM + n + 0.5f);  ib = (int)(b * DIM + n + 0.5f);
            if (ir < 0) ir = 0;
            if (ig < 0) ig = 0;
            if (ib < 0) ib = 0;
            g_dim[(size_t)py * w + px] = 0xFF000000u | (uint32_t)ir << 16 | (uint32_t)ig << 8 | (uint32_t)ib;
        }
    }
}

static void spawn(Speck *s, int anywhere)
{
    float b = frand();

    s->blur = b * b;                                /* more sharp ones than blurry ones */
    s->x = frand() * 1280.0f;
    s->y = anywhere ? frand() * 720.0f : 720.0f + 30.0f;
    s->speed = (26.0f - 19.0f * s->blur) * (0.6f + 0.7f * frand());
    s->sway = 4.0f + 14.0f * frand();
    s->sway_rate = 0.15f + 0.35f * frand();
    s->phase = frand() * 6.2832f;
    s->twinkle = 0.3f + 0.9f * frand();
}

/* Adds one speck of light at a sub-pixel position. */
static void draw_speck(uint32_t *fb, int W, int H, float cx, float cy, float radius, float edge, float strength)
{
    int x0 = (int)floorf(cx - radius - 1), x1 = (int)ceilf(cx + radius + 1);
    int y0 = (int)floorf(cy - radius - 1), y1 = (int)ceilf(cy + radius + 1);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;

    for (int y = y0; y <= y1; y++) {
        uint32_t *row = fb + (size_t)y * W;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float a = (radius - sqrtf(dx * dx + dy * dy)) / edge;
            if (a <= 0.0f)
                continue;
            if (a > 1.0f)
                a = 1.0f;
            a = a * a * (3.0f - 2.0f * a) * strength;

            uint32_t c = row[x];
            int r = (int)((c >> 16) & 255) + (int)(190.0f * a);
            int g = (int)((c >> 8) & 255) + (int)(222.0f * a);
            int b = (int)(c & 255) + (int)(255.0f * a);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            row[x] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
}

void backdrop_draw(uint32_t *fb, int W, int H)
{
    uint64_t now = plat_time_us();
    float dt = g_last ? (float)(now - g_last) / 1e6f : 0.0f;
    float t = (float)(now % 3600000000ull) / 1e6f;
    float scale = (float)H / 720.0f;

    g_last = now;
    if (dt > 0.1f)                                  /* coming back from a film: no jump */
        dt = 0.1f;

    if (!g_still || W != g_w || H != g_h)
        paint_still(W, H);
    if (!g_seeded) {
        for (int i = 0; i < SPECKS; i++)
            spawn(&g_specks[i], 1);
        g_seeded = 1;
    }
    memcpy(fb, g_still, (size_t)W * H * 4);

    for (int i = 0; i < SPECKS; i++) {
        Speck *s = &g_specks[i];

        s->y -= s->speed * dt;
        if (s->y < -30.0f)
            spawn(s, 0);

        float radius = 1.3f + 10.0f * s->blur;
        float edge = 1.0f + 1.1f * radius * s->blur;            /* width of the soft rim */
        float strength = 0.70f - 0.58f * sqrtf(s->blur);        /* out of focus = fainter */

        strength *= 0.65f + 0.35f * sinf(t * s->twinkle + s->phase);
        if (s->y > 640.0f)                                      /* fade in at the bottom ... */
            strength *= (720.0f + 30.0f - s->y) / 110.0f;
        if (s->y < 90.0f)                                       /* ... and out at the top */
            strength *= (s->y + 30.0f) / 120.0f;
        if (strength <= 0.0f)
            continue;

        float x = s->x + s->sway * sinf(t * s->sway_rate + s->phase);
        draw_speck(fb, W, H, x * (float)W / 1280.0f, s->y * scale,
                   radius * scale, edge * scale, strength);
    }
}

void backdrop_shade(uint32_t *fb, int W, int H, int x, int y, int w, int h)
{
    if (!g_still || W != g_w || H != g_h)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int row = 0; row < h; row++) {
        size_t at = (size_t)(y + row) * W + x;
        uint32_t *p = fb + at;
        const uint32_t *still = g_still + at, *dim = g_dim + at;

        for (int col = 0; col < w; col++) {
            uint32_t c = p[col], d = dim[col];
            if (c != still[col]) {                  /* dust here: keep 3/8 of its light */
                uint32_t s0 = still[col];
                uint32_t r = ((d >> 16) & 255) + ((((c >> 16) & 255) - ((s0 >> 16) & 255)) * 3 >> 3);
                uint32_t g = ((d >> 8) & 255) + ((((c >> 8) & 255) - ((s0 >> 8) & 255)) * 3 >> 3);
                uint32_t b = (d & 255) + (((c & 255) - (s0 & 255)) * 3 >> 3);
                d = 0xFF000000u | r << 16 | g << 8 | b;
            }
            p[col] = d;
        }
    }
}
