#ifndef PLAYER_PROBE_H
#define PLAYER_PROBE_H

/* Serial, read-only stage-3 probes; called by the single console/media task. */
void player_probe_list(const char *path);
void player_probe_media(const char *path);
void player_probe_video(const char *path, unsigned long frame_limit);
void player_probe_video_hold(const char *path);
void player_probe_video_verify(const char *path);
void player_probe_stop(void);

#endif
