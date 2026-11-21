/* A subtitle file that lies beside the video: same name, .srt extension. */
#ifndef EXTSUB_H
#define EXTSUB_H

typedef struct {
    double start, end;          /* seconds from the start of the video */
    char   text[400];
} ExtCue;

typedef struct {
    ExtCue *cues;               /* in file order */
    int     count;
} ExtSub;

/* Looks for the file that belongs to the video (a local path or an smb://
 * address) and reads it. Returns 0 with the lines in *sub, -1 if there is
 * no such file or nothing usable in it. */
int  extsub_load(const char *video, ExtSub *sub);
void extsub_free(ExtSub *sub);

#endif
