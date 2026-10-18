/* Exercise the production sampler with independent OST/Idle observations.
 * No RTOS, renderer, task enumeration, timer waits or board are used. */
#include "game_stats.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t clock_us, idle_us;
static unsigned critical_depth, idle_reads, clock_reads, failures;

static void expect(int condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL game_stats: %s\n", name); ++failures; }
}
void game_stats_test_enter(void) { ++critical_depth; }
void game_stats_test_exit(void)
{
    expect(critical_depth != 0, "critical sections balance");
    if (critical_depth) --critical_depth;
}
uint64_t platform_time_us(void) { ++clock_reads; return clock_us; }
uint64_t ulTaskGetIdleRunTimeCounter(void)
{
    expect(critical_depth != 0, "64-bit Idle counter read is protected");
    ++idle_reads;
    return idle_us;
}

static game_stats_t snapshot(void)
{
    game_stats_t stats;
    unsigned clocks = clock_reads, idles = idle_reads;
    game_stats_get(&stats);
    expect(!critical_depth, "snapshot releases its lock");
    expect(clocks == clock_reads && idles == idle_reads, "get is a snapshot, not a new sample");
    return stats;
}

static void begin_at(uint64_t now, uint64_t idle)
{
    clock_us = now; idle_us = idle;
    game_stats_begin();
    game_stats_t stats = snapshot();
    expect(stats.running == 1 && stats.valid == 0, "begin starts with unavailable metrics");
    expect(!stats.cpu_tenths && !stats.fps_tenths && !stats.window_ms && !stats.window_frames,
           "begin discards previous metrics");
    expect(!critical_depth, "begin releases its locks");
}

static void test_windows(void)
{
    begin_at(100000, 40000);
    unsigned idles = idle_reads;
    clock_us += 999999; idle_us += 250000;
    game_stats_frame(59);
    expect(!snapshot().valid && idle_reads == idles,
           "subsecond frames neither publish nor resample Idle");
    ++clock_us;
    game_stats_frame(60);
    game_stats_t stats = snapshot();
    expect(stats.valid && stats.running && stats.cpu_tenths == 750 && stats.fps_tenths == 600,
           "one-second sample measures 75.0 percent and 60.0 FPS");
    expect(stats.window_ms == 1000 && stats.window_frames == 60 && idle_reads == idles + 1,
           "first window includes all frames since begin");
    game_stats_t previous = stats;
    idles = idle_reads;
    clock_us += 500000; idle_us += 100000;
    game_stats_frame(90);
    stats = snapshot();
    expect(!memcmp(&previous, &stats, sizeof(stats)) && idle_reads == idles,
           "published window remains stable until another full second");
    clock_us += 500000; idle_us += 400000;
    game_stats_frame(120);
    stats = snapshot();
    expect(stats.cpu_tenths == 500 && stats.fps_tenths == 600 && stats.window_frames == 60,
           "next sample uses window deltas, not lifetime totals");
    expect(idle_reads == idles + 1 && !critical_depth, "one Idle sample per completed window");
}

static void test_rates(void)
{
    /* Literal expectations are derived from the synthetic workload, rather
     * than sharing the sampler's integer expression as an oracle. */
    static const struct {
        uint64_t elapsed, idle;
        uint32_t frames, cpu, fps, milliseconds;
    } cases[] = {
        {1000000,       0, 60, 1000, 600, 1000},
        {1000000, 1000000, 60,    0, 600, 1000},
        {1000000, 1000100, 60,    0, 600, 1000},
        {1000000,  500500, 50,  500, 500, 1000},
        {1000000,     500, 50, 1000, 500, 1000},
        {1200000,  333333, 72,  722, 600, 1200},
        {1003000,  703000, 60,  299, 598, 1003},
        {4000000, 3000000,239,  250, 598, 4000},
        {2000000, 1000000, 60,  500, 300, 2000}
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint64_t base_clock = (UINT64_C(1) << 40) + i;
        uint64_t base_idle = (UINT64_C(1) << 39) + i;
        begin_at(base_clock, base_idle);
        clock_us += cases[i].elapsed; idle_us += cases[i].idle;
        game_stats_frame(cases[i].frames);
        game_stats_t stats = snapshot();
        if (stats.cpu_tenths != cases[i].cpu || stats.fps_tenths != cases[i].fps ||
            stats.window_ms != cases[i].milliseconds || stats.window_frames != cases[i].frames) {
            fprintf(stderr, "FAIL game_stats rate case=%u CPU=%u FPS=%u window=%u frames=%u\n",
                    i, stats.cpu_tenths, stats.fps_tenths, stats.window_ms, stats.window_frames);
            ++failures;
        }
        expect(stats.valid && stats.running, "rate sample is valid/running");
    }
}

static void test_pause_resume(void)
{
    begin_at(10000000, 2000000);
    clock_us += 1000000; idle_us += 250000; game_stats_frame(60);
    game_stats_t before = snapshot();
    clock_us += 400000; idle_us += 100000;
    game_stats_end();
    game_stats_t stopped = snapshot();
    expect(!stopped.running && stopped.valid && stopped.cpu_tenths == before.cpu_tenths &&
           stopped.fps_tenths == before.fps_tenths && stopped.window_frames == before.window_frames &&
           stopped.window_ms == before.window_ms, "end freezes last complete window without publishing tail");
    unsigned idles = idle_reads;
    clock_us += UINT64_C(600000000); idle_us += UINT64_C(550000000);
    game_stats_frame(1000);
    game_stats_t after = snapshot();
    expect(!memcmp(&stopped, &after, sizeof(after)) && idle_reads == idles,
           "paused frames do not change metrics or sample Idle");
    begin_at(clock_us, idle_us);
    clock_us += 1000000; idle_us += 750000; game_stats_frame(60);
    after = snapshot();
    expect(after.cpu_tenths == 250 && after.fps_tenths == 600 && after.window_frames == 60,
           "resume excludes paused/menu time and prior frame count");
    begin_at(clock_us, idle_us);
    clock_us += 500000; idle_us += 100000; game_stats_frame(30); game_stats_end();
    after = snapshot();
    expect(!after.running && !after.valid && !after.window_frames && !after.fps_tenths,
           "short run ends with no fabricated sample");
}

static void test_frame_wrap(void)
{
    begin_at(0, 0);
    /* Seed a cumulative count near uint32 wrap through the public API. The
     * first interval is only setup; the measured next interval is 60 frames. */
    clock_us = UINT64_C(60000000); idle_us = UINT64_C(30000000);
    game_stats_frame(UINT32_MAX - 30u);
    clock_us += 1000000; idle_us += 250000;
    game_stats_frame(29);
    game_stats_t stats = snapshot();
    expect(stats.window_frames == 60 && stats.fps_tenths == 600 && stats.cpu_tenths == 750,
           "completed frame count wraps with unsigned delta");
}

int main(void)
{
    test_windows(); test_rates(); test_pause_resume(); test_frame_wrap();
    if (failures) return 1;
    puts("game_stats PASS: CPU/Idle and FPS rates, rounding, one-second windows, 64-bit counters, pause/resume, frame wrap, snapshot locks");
    return 0;
}
