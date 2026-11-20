/* Start-up and the main loop: pick a video in the list, play it, repeat. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "browser.h"
#include "osd.h"
#include "plat.h"
#include "player.h"
#include "smb.h"

#define CONFIG_DIR   "/data/orbismozi"
#define SHARES_FILE  CONFIG_DIR "/shares.txt"

/* Creates our folders and, the first time, an example shares file that the
 * user can fetch over FTP, edit and put back. */
static void prepare_folders(void)
{
    static const char example[] =
        "# Orbis Mozi - network shares\n"
        "#\n"
        "# One share per line. SMB2 or SMB3 is needed (not the old SMB1).\n"
        "# Lines starting with # are ignored.\n"
        "#\n"
        "#   smb://user:password@192.168.1.10/share\n"
        "#   smb://192.168.1.10/public/films          (guest access, starts in a folder)\n";
    FILE *f;

    if (mkdir("/data/video", 0777) != 0 && errno != EEXIST)
        osd_log("cannot create /data/video (%s)", strerror(errno));
    if (mkdir(CONFIG_DIR, 0777) != 0 && errno != EEXIST)
        osd_log("cannot create " CONFIG_DIR " (%s)", strerror(errno));

    if ((f = fopen(SHARES_FILE, "r")) != NULL) {
        fclose(f);
    } else if ((f = fopen(SHARES_FILE, "w")) != NULL) {
        fwrite(example, 1, sizeof(example) - 1, f);
        fclose(f);
    }
}

/* Shows the log until cross or circle is pressed. */
static void show_log_until_button(void)
{
    unsigned prev = plat_pad();

    osd_log("press X to go back");
    for (;;) {
        osd_show_log();
        unsigned pad = plat_pad();
        if ((pad & ~prev) & (PAD_CROSS | PAD_CIRCLE))
            return;
        prev = pad;
    }
}

static void report(int rc, const PlayerStats *stats)
{
    osd_log(rc == 0 ? "playback finished" : "playback failed");
    osd_log("played %.1f s, %d frames shown, %d dropped, convert %.1f ms/frame, worst a/v offset %.0f ms",
            stats->seconds_played, stats->frames_shown, stats->frames_dropped,
            stats->avg_convert_ms, stats->max_abs_av_diff_ms);
}

int main(int argc, char **argv)
{
    char path[1500] = "";
    PlayerStats stats;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (plat_init() != 0) {
        printf("[mozi] platform init failed\n");
        for (;;)
            plat_sleep_us(1000000);
    }
    osd_log("Orbis Mozi - screen %dx%d", plat_width(), plat_height());

    if (argc > 1) {                                 /* PC test: play one file and exit */
        smb_display_name(argv[1], path, sizeof(path));     /* keeps a password out of the log */
        osd_log("playing %s", path);
        int rc = player_run(argv[1], argc > 2 ? atof(argv[2]) : 0, &stats);
        report(rc, &stats);
        return rc == 0 ? 0 : 1;
    }

    if (!getenv("MOZI_LOCAL")) {                    /* set only when testing on a PC */
        prepare_folders();
        osd_log_to_file(CONFIG_DIR "/log.txt");
    }

    for (;;) {
        browser_pick(path, sizeof(path));
        osd_log("playing %s", smb_is_url(path) ? "from a network share" : path);

        int rc = player_run(path, 0, &stats);
        report(rc, &stats);
        if (rc != 0)
            show_log_until_button();                /* otherwise the reason would be lost */
    }
}
