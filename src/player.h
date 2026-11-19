#ifndef PLAYER_H
#define PLAYER_H

typedef struct {
    double seconds_played;
    int    frames_shown;
    int    frames_dropped;
    double avg_convert_ms;      /* colour conversion + scaling per frame   */
    double max_abs_av_diff_ms;  /* worst video-vs-audio offset when shown  */
} PlayerStats;

/* Plays one file from start to end (or for max_seconds if > 0).
 * Returns 0 on success, negative if the file could not be played. */
int player_run(const char *path, double max_seconds, PlayerStats *stats);

#endif
