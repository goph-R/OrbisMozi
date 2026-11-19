/* Start-up: find a video file, play it, report on screen. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "osd.h"
#include "plat.h"
#include "player.h"

/* Searched in this order; the first video found is played. */
static const char *g_dirs[] = {
    "/mnt/usb0", "/mnt/usb1", "/usb0", "/usb1",
    "/data/video", "/app0/assets/videos",
};

static int is_video(const char *name)
{
    static const char *exts[] = { ".mkv", ".mp4", ".avi", ".m4v", ".mov", ".ts", ".webm", ".mpg", ".wmv", ".flv" };
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.')
        return 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (strcasecmp(dot, exts[i]) == 0)
            return 1;
    return 0;
}

/* Finds the alphabetically first video in dir. Returns 1 if found. */
static int find_in_dir(const char *dir, char *out, size_t out_size)
{
    char best[512] = "";
    int entries = 0;
    DIR *d = opendir(dir);

    if (!d) {
        osd_log("  %-20s not accessible (%s)", dir, strerror(errno));
        return 0;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        entries++;
        if (is_video(e->d_name) && (!best[0] || strcasecmp(e->d_name, best) < 0))
            snprintf(best, sizeof(best), "%s", e->d_name);
    }
    closedir(d);

    if (!best[0]) {
        osd_log("  %-20s %d entries, no video", dir, entries);
        return 0;
    }
    osd_log("  %-20s %d entries, found %s", dir, entries, best);
    snprintf(out, out_size, "%s/%s", dir, best);
    return 1;
}

/* Logs the names in a directory on one line, to show what the app can see. */
static void list_dir(const char *dir)
{
    char line[140];
    int len, entries = 0;
    DIR *d = opendir(dir);

    if (!d) {
        osd_log("  %s: not accessible (%s)", dir, strerror(errno));
        return;
    }
    len = snprintf(line, sizeof(line), "  %s:", dir);
    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        entries++;
        if (len + (int)strlen(e->d_name) + 2 < (int)sizeof(line))
            len += snprintf(line + len, sizeof(line) - len, " %s", e->d_name);
    }
    closedir(d);
    if (entries == 0)
        snprintf(line + len, sizeof(line) - len, " (empty)");
    osd_log("%s", line);
}

static void hold_log(unsigned seconds)
{
    for (unsigned i = 0; i < seconds * 10; i++) {
        osd_show_log();
        plat_sleep_us(100000);
    }
}

int main(int argc, char **argv)
{
    char path[1024] = "";
    PlayerStats stats;
    double max_seconds = 0;
    int found = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (plat_init() != 0) {
        printf("[mozi] platform init failed\n");
        for (;;)
            plat_sleep_us(1000000);
    }
    osd_log("Orbis Mozi test build - screen %dx%d", plat_width(), plat_height());

    if (argc > 1) {                                 /* PC test: file on the command line */
        snprintf(path, sizeof(path), "%s", argv[1]);
        if (argc > 2)
            max_seconds = atof(argv[2]);
        found = 1;
    } else {
        /* Our own folder on the console's disk; files can be put there over FTP. */
        if (mkdir("/data/video", 0777) != 0 && errno != EEXIST)
            osd_log("cannot create /data/video (%s)", strerror(errno));
        osd_log("folders this app can see:");
        list_dir("/");
        list_dir("/data");
        osd_log("looking for a video file:");
        for (size_t i = 0; i < sizeof(g_dirs) / sizeof(g_dirs[0]) && !found; i++)
            found = find_in_dir(g_dirs[i], path, sizeof(path));
    }

    if (!found) {
        osd_log("no video file found - nothing to play");
    } else {
        osd_log("playing %s", path);
        if (argc <= 1)
            hold_log(8);                           /* time to read the log */

        int rc = player_run(path, max_seconds, &stats);
        osd_log(rc == 0 ? "playback finished" : "playback failed");
        osd_log("played %.1f s, %d frames shown, %d dropped, convert %.1f ms/frame, worst a/v offset %.0f ms",
                stats.seconds_played, stats.frames_shown, stats.frames_dropped,
                stats.avg_convert_ms, stats.max_abs_av_diff_ms);
    }

    if (argc > 1)
        return 0;

    for (;;)                                        /* keep the result on screen */
        hold_log(1);
}
