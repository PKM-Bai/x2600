#ifndef PLAYER_PROBE_H
#define PLAYER_PROBE_H

#include <stdint.h>

/* Serial, read-only stage-3 probes; called by the single console/media task. */
void player_probe_list(const char *path);
void player_probe_media(const char *path);
/* Same storage worker, with audio stopped. maxbytes=0 selects 8MiB; otherwise
 * read at most maxbytes from the file start, stopping at its declared size.
 * Read-only TF path only; @embedded is rejected. Prints measured wall/I/O
 * time and bytes/s, including per-block yields in the wall-time result. */
void player_probe_benchmark(const char *path, uint32_t maxbytes);
void player_probe_video(const char *path, unsigned long frame_limit);
void player_probe_video_hold(const char *path);
void player_probe_video_verify(const char *path);
void player_probe_stop(void);

#endif
