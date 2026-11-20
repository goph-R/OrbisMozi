/* PS4 back end: sceVideoOut frame buffers and sceAudioOut. */
#include <fcntl.h>
#include <stddef.h>
#include <wchar.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/AudioOut.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include "osd.h"
#include "plat.h"

#define NUM_BUFFERS   2
#define BUFFER_STRIDE 0x800000          /* 8 MB each: enough for 1920x1080x4 */
#define VIDEO_MEM     (BUFFER_STRIDE * NUM_BUFFERS)

static int               g_video, g_audio = -1, g_pad = -1, g_user;
static int               g_w, g_h;
static OrbisKernelEqueue g_flip_queue;
static void             *g_buffers[NUM_BUFFERS];
static int               g_back;            /* index of the buffer being drawn */
static int64_t           g_flip_id;

/* FFmpeg starts its worker threads with the default stack size. Give every
 * thread a known, generous stack instead of relying on the system default.
 * (The linker redirects pthread_create here via --wrap.) */
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    pthread_attr_t own;
    int rc;

    if (attr)
        return __real_pthread_create(thread, attr, fn, arg);

    pthread_attr_init(&own);
    pthread_attr_setstacksize(&own, 2 * 1024 * 1024);
    rc = __real_pthread_create(thread, &own, fn, arg);
    pthread_attr_destroy(&own);
    return rc;
}

/* fcntl(F_SETFL, O_NONBLOCK) is refused for sockets on the console (EACCES),
 * which would leave libsmb2 with a blocking socket and hang it. Do the same
 * through the socket option the PS4 has for it, or FIONBIO as a last resort.
 * (The linker redirects fcntl here via --wrap.) */
#define PS4_SO_NBIO     0x1200
#define FREEBSD_FIONBIO 0x8004667eUL

int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    unsigned long arg;

    va_start(ap, cmd);
    arg = va_arg(ap, unsigned long);
    va_end(ap);

    int rc = __real_fcntl(fd, cmd, arg);
    if (rc < 0 && cmd == F_SETFL) {
        int on = (arg & O_NONBLOCK) != 0;
        if (setsockopt(fd, SOL_SOCKET, PS4_SO_NBIO, &on, sizeof(on)) == 0)
            return 0;
        if (ioctl(fd, FREEBSD_FIONBIO, &on) == 0)
            return 0;
    }
    return rc;
}

static int register_buffers(int w, int h)
{
    OrbisVideoOutBufferAttribute attr;

    /* 0x80000000 = A8R8G8B8 sRGB, 1 = linear layout. */
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, w, h, w);
    return sceVideoOutRegisterBuffers(g_video, 0, g_buffers, NUM_BUFFERS, &attr);
}

static int g_set = -1;                  /* handle of the registered buffer set */

int plat_set_mode(int w, int h)
{
    int old_w = g_w, old_h = g_h, rc;

    if (w == g_w && h == g_h)
        return 0;

    /* Drop the current buffers and register the same memory at the new size. */
    if (g_set >= 0)
        sceVideoOutUnregisterBuffers(g_video, g_set);
    rc = register_buffers(w, h);

    if (rc < 0) {
        /* Second attempt: reopen the video output completely. */
        osd_log("re-registering buffers failed (0x%08X), reopening video out", (unsigned)rc);
        sceVideoOutClose(g_video);
        g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
        sceVideoOutAddFlipEvent(g_flip_queue, g_video, 0);
        rc = register_buffers(w, h);
        if (rc < 0) {
            osd_log("cannot switch to %dx%d (0x%08X), staying at %dx%d", w, h, (unsigned)rc, old_w, old_h);
            g_set = register_buffers(old_w, old_h);
            sceVideoOutSetFlipRate(g_video, 0);
            return -1;
        }
        sceVideoOutSetFlipRate(g_video, 0);
    }

    g_set = rc;
    g_w = w;
    g_h = h;
    g_back = 0;
    memset(g_buffers[0], 0, VIDEO_MEM);
    return 0;
}

int plat_init(void)
{
    off_t phys = 0;
    void *mem = NULL;
    int rc;

    sceUserServiceInitialize(NULL);

    /* Controller of the user who started the application. */
    if (scePadInit() == 0 && sceUserServiceGetInitialUser(&g_user) == 0)
        g_pad = scePadOpen(g_user, 0, 0, NULL);
    if (g_pad < 0)
        osd_log("cannot open the controller (0x%08X)", (unsigned)g_pad);

    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g_video < 0)
        return -1;

    if (sceKernelCreateEqueue(&g_flip_queue, "player flip queue") < 0)
        return -1;
    sceVideoOutAddFlipEvent(g_flip_queue, g_video, 0);

    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), VIDEO_MEM, 0x200000, 3, &phys) < 0)
        return -1;
    if (sceKernelMapDirectMemory(&mem, VIDEO_MEM, 0x33, 0, phys, 0x200000) < 0)
        return -1;
    for (int i = 0; i < NUM_BUFFERS; i++)
        g_buffers[i] = (char *)mem + (size_t)i * BUFFER_STRIDE;
    memset(mem, 0, VIDEO_MEM);

    /* 720p buffers mean less work per frame; the console scales to the TV.
     * Fall back to 1080p (known to work) if they are refused. */
    g_w = 1280; g_h = 720;
    rc = register_buffers(g_w, g_h);
    if (rc < 0) {
        int first = rc;
        g_w = 1920; g_h = 1080;
        rc = register_buffers(g_w, g_h);
        if (rc < 0)
            return -1;
        osd_log("720p buffers refused (0x%08X), using 1080p", (unsigned)first);
    }
    g_set = rc;
    sceVideoOutSetFlipRate(g_video, 0);

    /* Audio is optional: without it the player runs on the wall clock. */
    if (sceAudioOutInit() != 0)
        osd_log("sceAudioOutInit reported an error (may already be initialised)");
    g_audio = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                              AUDIO_GRAIN, AUDIO_RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (g_audio <= 0)
        osd_log("sceAudioOutOpen failed (0x%08X)", (unsigned)g_audio);

    return 0;
}

int plat_width(void)  { return g_w; }
int plat_height(void) { return g_h; }

uint32_t *plat_backbuffer(void)
{
    return (uint32_t *)g_buffers[g_back];
}

void plat_flip(void)
{
    int64_t id = ++g_flip_id;

    sceVideoOutSubmitFlip(g_video, g_back, ORBIS_VIDEO_OUT_FLIP_VSYNC, id);
    for (;;) {
        OrbisVideoOutFlipStatus status;
        OrbisKernelEvent evt;
        int count;

        sceVideoOutGetFlipStatus(g_video, &status);
        if (status.flipArg == id)
            break;
        if (sceKernelWaitEqueue(g_flip_queue, &evt, 1, &count, 0) != 0)
            break;
    }
    g_back = (g_back + 1) % NUM_BUFFERS;
}

void plat_audio_write(const int16_t *pcm)
{
    if (g_audio <= 0 || sceAudioOutOutput(g_audio, pcm) < 0)
        sceKernelUsleep((unsigned)((uint64_t)AUDIO_GRAIN * 1000000 / AUDIO_RATE));   /* keep real-time pace */
}

unsigned plat_pad(void)
{
    static const struct { unsigned sce, own; } map[] = {
        { ORBIS_PAD_BUTTON_UP, PAD_UP },             { ORBIS_PAD_BUTTON_DOWN, PAD_DOWN },
        { ORBIS_PAD_BUTTON_LEFT, PAD_LEFT },         { ORBIS_PAD_BUTTON_RIGHT, PAD_RIGHT },
        { ORBIS_PAD_BUTTON_CROSS, PAD_CROSS },       { ORBIS_PAD_BUTTON_CIRCLE, PAD_CIRCLE },
        { ORBIS_PAD_BUTTON_TRIANGLE, PAD_TRIANGLE }, { ORBIS_PAD_BUTTON_SQUARE, PAD_SQUARE },
        { ORBIS_PAD_BUTTON_L1, PAD_L1 },             { ORBIS_PAD_BUTTON_R1, PAD_R1 },
        { ORBIS_PAD_BUTTON_OPTIONS, PAD_OPTIONS },
    };
    OrbisPadData data;
    unsigned out = 0;

    if (g_pad < 0 || scePadReadState(g_pad, &data) != 0 || !data.connected)
        return 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (data.buttons & map[i].sce)
            out |= map[i].own;
    return out;
}

/* --- on-screen keyboard (the system's IME dialog; text is UTF-16) -------- */

_Static_assert(sizeof(wchar_t) == 2, "the IME dialog takes UTF-16 text");

#define IME_OPTION_PASSWORD 0x00000004u
#define IME_MAX_CHARS       255

static void utf8_to_utf16(const char *in, wchar_t *out, int out_cap)
{
    const unsigned char *p = (const unsigned char *)in;
    int n = 0;

    while (*p && n < out_cap - 1) {
        unsigned c = *p++, cp = c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra)
            cp = c & (0x3F >> extra);
        for (; extra > 0 && (*p & 0xC0) == 0x80; extra--)
            cp = (cp << 6) | (*p++ & 0x3F);
        out[n++] = cp < 0x10000 ? (wchar_t)cp : (wchar_t)'?';
    }
    out[n] = 0;
}

static void utf16_to_utf8(const wchar_t *in, char *out, int out_cap)
{
    int n = 0;

    for (; *in && n < out_cap - 4; in++) {
        unsigned cp = (unsigned short)*in;
        if (cp < 0x80) {
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = '\0';
}

int plat_keyboard(const char *title, char *buf, int buf_size, int password, void (*redraw)(void))
{
    static int module = 1;                          /* 1 = not tried yet */
    static wchar_t text[IME_MAX_CHARS + 1], wtitle[64];
    OrbisImeDialogSetting param;
    OrbisDialogResult result;
    int rc, max_chars = buf_size - 1 < IME_MAX_CHARS ? buf_size - 1 : IME_MAX_CHARS;

    if (module == 1) {
        module = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
        if (module != 0)
            osd_log("system keyboard not available (0x%08X), using the built-in one", (unsigned)module);
    }
    if (module != 0)
        return -1;

    utf8_to_utf16(buf, text, max_chars + 1);
    utf8_to_utf16(title, wtitle, 64);

    memset(&param, 0, sizeof(param));
    param.userId = (uint32_t)g_user;
    param.type = ORBIS_TYPE_BASIC_LATIN;
    param.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    param.option = password ? IME_OPTION_PASSWORD : 0;
    param.maxTextLength = (uint32_t)max_chars;
    param.inputTextBuffer = text;
    param.posx = 960.0f;                            /* centre of the dialog, in 1920x1080 units */
    param.posy = 540.0f;
    param.horizontalAlignment = ORBIS_H_CENTER;
    param.verticalAlignment = ORBIS_V_CENTER;
    param.title = wtitle;

    rc = sceImeDialogInit(&param, NULL);
    if (rc < 0) {
        osd_log("system keyboard failed to open (0x%08X), using the built-in one", (unsigned)rc);
        module = rc;
        return -1;
    }

    for (;;) {
        OrbisDialogStatus status = sceImeDialogGetStatus();
        if (status == ORBIS_DIALOG_STATUS_STOPPED)
            break;
        if (status == ORBIS_DIALOG_STATUS_NONE) {   /* it never came up */
            sceImeDialogTerm();
            osd_log("system keyboard did not start, using the built-in one");
            module = -1;
            return -1;
        }
        redraw();                                   /* keep the screen alive behind the dialog */
    }

    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    if (result.endstatus != ORBIS_DIALOG_OK)
        return 0;
    utf16_to_utf8(text, buf, buf_size);
    return 1;
}

uint64_t plat_time_us(void)
{
    return sceKernelGetProcessTime();
}

void plat_sleep_us(unsigned us)
{
    sceKernelUsleep(us);
}
