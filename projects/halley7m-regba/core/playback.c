#include "playback.h"

int playback_should_render(uint32_t completed_frames, unsigned frameskip)
{
    return completed_frames % (frameskip + 1u) == 0;
}

uint64_t playback_target_us(uint32_t frames, unsigned speed)
{
    uint64_t cycles = (uint64_t)frames * 280896u;
    uint64_t hz = (uint64_t)16777216u * speed;
    return cycles / hz * 1000000u + cycles % hz * 1000000u / hz;
}
