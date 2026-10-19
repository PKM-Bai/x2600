#include "player_audio_probe.h"
#include "player_audio_client.h"
#include "player_platform.h"
#include "player_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <math.h>
#include <stdio.h>

void player_audio_probe_status(void)
{
    player_audio_client_stats s;
    player_audio_client_get_status(&s);
    printf("AUDIO_STATUS session=%lu state=%lu active=%lu total=%lu written=%lu completed=%lu underruns=%lu volume=%lu clock_valid=%lu error=%ld dma_errors=%lu dma_timeouts=%lu queue_underruns=%lu fifo_underruns=%lu\n",
           (unsigned long)s.media_session_id, (unsigned long)s.state,
           (unsigned long)s.session_active, (unsigned long)s.total_samples,
           (unsigned long)s.written_samples, (unsigned long)s.completed_samples,
           (unsigned long)s.underruns, (unsigned long)s.volume_percent,
           (unsigned long)s.clock_valid, (long)s.last_error,
           (unsigned long)s.dma_errors, (unsigned long)s.dma_timeouts,
           (unsigned long)s.queue_underruns, (unsigned long)s.fifo_underruns);
}

void player_audio_probe_tone(unsigned samples, int pause_test)
{
    int16_t period[480], wave[96];
    player_audio_client_stats s;
    unsigned written = 0;
    int paused_once = 0;
    player_audio_client_get_status(&s);
    if (s.session_active) { printf("TONE_BUSY stop-current-session-first\n"); return; }
    if (!samples || samples > 480000u) {
        printf("TONE_ARGUMENT samples=1..480000\n");
        return;
    }
    for (unsigned i = 0; i < 96; ++i)
        wave[i] = (int16_t)(8192.0 * sin(6.283185307179586 * i / 96.0));
    printf("TONE_BEGIN samples=%u rate=48000 hz=500 volume_percent=%u pause_test=%d\n",
           samples, PLAYER_DEFAULT_VOLUME_PERCENT, pause_test);
    uint64_t start = platform_time_us();
    int result = player_audio_client_start(samples, PLAYER_DEFAULT_VOLUME_PERCENT);
    if (result) goto done;
    while (written < samples) {
        unsigned count = samples - written;
        if (count > 480) count = 480;
        for (unsigned i = 0; i < count; ++i) period[i] = wave[(written + i) % 96u];
        written += player_audio_client_write_pcm(period, count);
        result = player_audio_client_service();
        if (result) goto done;
        player_audio_client_get_status(&s);
        if (s.last_error) { result = s.last_error; goto done; }
        if (platform_time_us() - start > UINT64_C(15000000)) { result = -12; goto done; }
        vTaskDelay(1);
    }
    result = player_audio_client_mark_eof();
    if (result) goto done;
    for (;;) {
        player_amp_service();
        result = player_audio_client_service();
        if (result) break;
        player_audio_client_get_status(&s);
        if (s.last_error) { result = s.last_error; break; }
        if (s.state == PLAYER_AMP_AUDIO_DRAINED) {
            result = s.completed_samples == samples && !s.underruns &&
                     !s.dma_errors && !s.dma_timeouts ? 0 : -20;
            break;
        }
        if (pause_test && !paused_once && s.completed_samples >= samples / 4u) {
            result = player_audio_client_pause();
            if (result) break;
            player_audio_client_get_status(&s);
            uint32_t frozen = s.completed_samples;
            uint64_t until = platform_time_us() + 100000u;
            while (platform_time_us() < until) {
                player_amp_service();
                if (player_audio_client_service()) { result = -20; break; }
                vTaskDelay(1);
            }
            player_audio_client_get_status(&s);
            if (!result && (s.state != PLAYER_AMP_AUDIO_PAUSED || s.completed_samples != frozen)) result = -20;
            printf("TONE_PAUSE samples_before=%lu samples_after=%lu result=%d\n",
                   (unsigned long)frozen, (unsigned long)s.completed_samples, result);
            if (result) break;
            result = player_audio_client_resume();
            if (result) break;
            paused_once = 1;
        }
        if (platform_time_us() - start > UINT64_C(15000000)) { result = -12; break; }
        int c = platform_uart_getchar();
        if (c == 'q' || c == 'Q' || c == 3) { result = -21; break; }
        vTaskDelay(1);
    }
done:
    player_audio_probe_status();
    int stopped = player_audio_client_stop();
    printf("TONE_END result=%d stop=%d written=%u elapsed_us=%llu heap=%lu\n",
           result, stopped, written, (unsigned long long)(platform_time_us() - start),
           (unsigned long)platform_free_heap());
}
