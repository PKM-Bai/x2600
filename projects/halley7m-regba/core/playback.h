#ifndef REGBA_PLAYBACK_H
#define REGBA_PLAYBACK_H

#include <stdint.h>

/* Called throughout one scanline frame with the same completed-frame count.
 * frameskip is 0..3: draw one frame, then skip that many frames. */
int playback_should_render(uint32_t completed_frames, unsigned frameskip);
/* Wall-clock target since the start/resume of a run. Speed is 1, 2 or 4.
 * The quotient/remainder split keeps long runs within uint64_t bounds. */
uint64_t playback_target_us(uint32_t frames, unsigned speed);

#endif
