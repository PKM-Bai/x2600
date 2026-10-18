/* X2600 ReGBA in-game performance counters. GPL-2.0-or-later. */
#ifndef REGBA_GAME_STATS_H
#define REGBA_GAME_STATS_H

#include <stdint.h>

typedef struct {
    /* CPU is the whole-system non-Idle share of FreeRTOS runtime. FPS is
     * completed GBA emulation frames per wall second, independent of LCD
     * presentation/skips. Both values are rounded to tenths. */
    uint32_t cpu_tenths, fps_tenths;
    uint32_t window_ms, window_frames;
    uint32_t valid, running;
} game_stats_t;

/* Core task only: begin a fresh run/resume, report committed GBA frames,
 * then end before menu/save work. Publish at most once per >=1s window;
 * no allocation, task enumeration, or waits. Frame counts start at zero
 * for each begin. End retains the last complete window with running=0. */
void game_stats_begin(void);
void game_stats_frame(uint32_t completed_frames);
void game_stats_end(void);
/* Atomic snapshot for the core renderer or console; never samples. */
void game_stats_get(game_stats_t *stats);

#endif
