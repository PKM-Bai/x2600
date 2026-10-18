/* X2600 ReGBA in-game performance counters. GPL-2.0-or-later. */
#include "game_stats.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"

static game_stats_t published;
static uint64_t sample_us, sample_idle;
static uint32_t sample_frames;

static void runtime_snapshot(uint64_t *now, uint64_t *idle)
{
    /* On this 32-bit CPU the existing runtime counters are 64-bit. A short
     * critical section prevents a task switch from tearing their snapshot.
     * The caller is the non-Idle core task, so Idle's counter is committed. */
    taskENTER_CRITICAL();
    *now = platform_time_us();
    *idle = ulTaskGetIdleRunTimeCounter();
    taskEXIT_CRITICAL();
}

void game_stats_begin(void)
{
    runtime_snapshot(&sample_us, &sample_idle);
    sample_frames = 0;
    taskENTER_CRITICAL();
    published = (game_stats_t){ .running = 1 };
    taskEXIT_CRITICAL();
}

void game_stats_frame(uint32_t completed_frames)
{
    if (!published.running || platform_time_us() - sample_us < 1000000u)
        return;
    uint64_t now, idle;
    runtime_snapshot(&now, &idle);
    uint64_t elapsed = now - sample_us;
    uint64_t idle_delta = idle - sample_idle;
    uint32_t frames = completed_frames - sample_frames;
    if (idle_delta > elapsed) idle_delta = elapsed;
    game_stats_t next = {
        .cpu_tenths = (uint32_t)(((elapsed - idle_delta) * 1000u + elapsed / 2u) / elapsed),
        .fps_tenths = (uint32_t)(((uint64_t)frames * 10000000u + elapsed / 2u) / elapsed),
        .window_ms = (uint32_t)(elapsed / 1000u),
        .window_frames = frames,
        .valid = 1, .running = 1
    };
    sample_us = now;
    sample_idle = idle;
    sample_frames = completed_frames;
    taskENTER_CRITICAL();
    published = next;
    taskEXIT_CRITICAL();
}

void game_stats_end(void)
{
    taskENTER_CRITICAL();
    published.running = 0;
    taskEXIT_CRITICAL();
}

void game_stats_get(game_stats_t *stats)
{
    taskENTER_CRITICAL();
    *stats = published;
    taskEXIT_CRITICAL();
}
