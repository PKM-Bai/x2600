/* The runner injects production client and remote service code. Hardware is
 * replaced with the real private PCM queue plus a deterministic DMA clock. */
#include "player_audio_client.h"
#include "player_audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
static uint64_t test_now;
static struct player_amp_shared test_shared;
static int16_t test_pcm[PLAYER_AMP_PCM_SAMPLES];
static volatile struct player_amp_shared *shared = &test_shared;
static player_pcm_queue hw_queue;
static player_audio_stats hw_stats;
static uint64_t hw_end;
static unsigned hw_pause, hw_primed, close_calls;
static int peer_blocked, bad_stop_ack, corrupt_event_crc, wrong_ack_sequence;
static int wrong_media_session, wrong_event_sequence;
static int stop_error_race;
static int force_silence_calls;
uint64_t platform_time_us(void) { return test_now; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(test_now / 1000u); }
void vTaskDelay(uint32_t ticks);
void player_amp_service(void);
const volatile struct player_amp_shared *player_amp_state(void) { return &test_shared; }
void player_subcore_force_silence(void)
{
    ++force_silence_calls;
    test_shared.host_state = test_shared.remote_state = PLAYER_AMP_FAULT;
}

int player_audio_open(void)
{
    hw_stats.state = PLAYER_AUDIO_IDLE;
    return 0;
}
int player_audio_begin(void)
{
    player_pcm_reset(&hw_queue);
    unsigned volume = hw_stats.volume_percent;
    memset(&hw_stats, 0, sizeof(hw_stats));
    hw_stats.volume_percent = volume;
    hw_stats.state = PLAYER_AUDIO_BUFFERING;
    hw_pause = hw_primed = 0;
    test_now += 300000;
    return 0;
}
int player_audio_service(void)
{
    if (hw_stats.state == PLAYER_AUDIO_CLOSED || hw_stats.state == PLAYER_AUDIO_IDLE) return 0;
    if (hw_stats.state == PLAYER_AUDIO_FAULT) return hw_stats.last_error;
    if (hw_queue.active_slot >= 0 && test_now >= hw_end) {
        const player_pcm_period *p = &hw_queue.period[hw_queue.active_slot];
        for (unsigned i = 0; i < p->valid_samples; ++i) {
            uint32_t at = hw_queue.completed_samples + i;
            assert(p->samples[i] == (int16_t)(at ^ (at >> 6)));
        }
        assert(player_pcm_complete(&hw_queue) == 0);
    }
    if (hw_queue.active_slot >= 0) return 0;
    if (hw_pause) { hw_stats.state = PLAYER_AUDIO_PAUSED; return 0; }
    if (player_pcm_drained(&hw_queue)) { hw_stats.state = PLAYER_AUDIO_DRAINED; return 0; }
    if (hw_queue.ready_count && (hw_primed || hw_queue.eof ||
        hw_queue.ready_count >= PLAYER_PCM_PRIME_PERIODS)) {
        assert(player_pcm_take(&hw_queue));
        hw_primed = 1;
        hw_end = test_now + 10000;
        hw_stats.state = PLAYER_AUDIO_PLAYING;
    } else hw_stats.state = PLAYER_AUDIO_BUFFERING;
    return 0;
}
unsigned player_audio_write(const int16_t *samples, unsigned count)
{
    (void)player_audio_service();
    unsigned accepted = player_pcm_write(&hw_queue, samples, count);
    (void)player_audio_service();
    return accepted;
}
int player_audio_finish(void) { player_pcm_finish(&hw_queue); return player_audio_service(); }
int player_audio_pause(void) { hw_pause = 1; return player_audio_service(); }
int player_audio_resume(void)
{
    if (hw_stats.state != PLAYER_AUDIO_PAUSED) return PLAYER_AUDIO_STATE;
    hw_pause = 0; hw_primed = 1;
    return player_audio_service();
}
int player_audio_set_volume(unsigned percent) { hw_stats.volume_percent = percent; return 0; }
int player_audio_stop(void)
{
    uint32_t submitted = hw_queue.submitted_samples, completed = hw_queue.completed_samples;
    player_pcm_reset(&hw_queue);
    hw_queue.submitted_samples = submitted; hw_queue.completed_samples = completed;
    hw_stats.state = PLAYER_AUDIO_IDLE;
    return 0;
}
int player_audio_close(void)
{
    ++close_calls;
    (void)player_audio_stop();
    hw_stats.state = PLAYER_AUDIO_CLOSED;
    return 0;
}
void player_audio_get_stats(player_audio_stats *output)
{
    *output = hw_stats;
    output->submitted_samples = hw_queue.submitted_samples;
    output->completed_samples = hw_queue.completed_samples;
    output->ready_periods = hw_queue.ready_count;
    output->active_period = hw_queue.active_slot;
}

/* @PRODUCTION_AUDIO_REMOTE@ */
/* @PRODUCTION_AUDIO_CLIENT@ */

static void peer_step(void)
{
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    if (peer_blocked) return;
    rv_audio_service();
    struct player_amp_ring *r = &test_shared.audio_remote_to_host;
    for (uint32_t i = r->read_pos; i != r->write_pos; ++i) {
        struct player_amp_message *e = &r->slots[i % PLAYER_AMP_RING_CAPACITY];
        if (bad_stop_ack && e->opcode == PLAYER_AMP_AUDIO_EVENT_ACK &&
            e->argument[1] == PLAYER_AMP_AUDIO_CMD_STOP) {
            e->argument[2] = PLAYER_AMP_AUDIO_PLAYING;
            bad_stop_ack = 0;
            e->checksum = player_amp_crc32(e, offsetof(struct player_amp_message, checksum));
        }
        if (wrong_ack_sequence && e->opcode == PLAYER_AMP_AUDIO_EVENT_ACK) {
            ++e->argument[0]; wrong_ack_sequence = 0;
            e->checksum = player_amp_crc32(e, offsetof(struct player_amp_message, checksum));
        }
        if (wrong_media_session) {
            ++e->session; wrong_media_session = 0;
            e->checksum = player_amp_crc32(e, offsetof(struct player_amp_message, checksum));
        }
        if (wrong_event_sequence) {
            ++e->sequence; wrong_event_sequence = 0;
            e->checksum = player_amp_crc32(e, offsetof(struct player_amp_message, checksum));
        }
        if (corrupt_event_crc) { e->checksum ^= 1u; corrupt_event_crc = 0; }
    }
#endif
}

void player_amp_service(void)
{
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    /* Inject a previous-command hardware ERROR after the client's pre-drain,
     * just as its newer STOP arrives. Production remote code then ACKs STOP.
     */
    if (stop_error_race && test_shared.audio_host_to_remote.write_pos !=
                          test_shared.audio_host_to_remote.read_pos) {
        const struct player_amp_message *m = &test_shared.audio_host_to_remote.slots[
            test_shared.audio_host_to_remote.read_pos % PLAYER_AMP_RING_CAPACITY];
        if (m->opcode == PLAYER_AMP_AUDIO_CMD_STOP) {
            rv_audio_fail(PLAYER_AUDIO_HARDWARE);
            assert(rv_audio_event(PLAYER_AMP_AUDIO_EVENT_ERROR, (uint32_t)PLAYER_AUDIO_HARDWARE,
                test_shared.audio_completed_frames, PLAYER_AMP_AUDIO_FAULT, rv_audio_command_sequence) == 1);
            rv_audio_error_pending = 0;
            stop_error_race = 0;
        }
    }
#endif
    peer_step();
}
void vTaskDelay(uint32_t ticks)
{
    test_now += (uint64_t)ticks * 1000u;
    peer_step();
}

static void reset_fixture(void)
{
    memset(&status, 0, sizeof(status));
    memset(&hw_stats, 0, sizeof(hw_stats));
    player_pcm_reset(&hw_queue);
    player_amp_initialize(&test_shared, 1, PLAYER_AUDIO_OWNER, 1, 4096);
    test_shared.host_state = test_shared.remote_state = PLAYER_AMP_RUNNING;
    next_media_session = 0;
    client_recovery_required = force_silence_calls = 0;
    peer_blocked = bad_stop_ack = corrupt_event_crc = wrong_ack_sequence = stop_error_race = 0;
    wrong_media_session = wrong_event_sequence = 0;
    test_now = hw_end = 0; close_calls = hw_pause = hw_primed = 0;
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    rv_audio_session = rv_audio_command_sequence = rv_audio_event_sequence = 0;
    rv_audio_total = rv_audio_copied = rv_audio_pause_sequence = 0;
    rv_audio_live = rv_audio_eof = rv_audio_finished = rv_audio_ended = 0;
    rv_audio_failed = rv_audio_error_pending = 0;
#endif
}

static void stream(unsigned total, int pause_once)
{
    static int16_t input[PLAYER_AMP_PCM_SAMPLES + 3503u];
    assert(total <= sizeof(input) / sizeof(input[0]));
    for (uint32_t i = 0; i < total; ++i) input[i] = (int16_t)(i ^ (i >> 6));
    assert(player_audio_client_start(total, 10) == 0);
    assert(player_audio_client_start(total, 10) == PLAYER_AUDIO_CLIENT_BUSY);
    assert(player_audio_client_mark_eof() == PLAYER_AUDIO_CLIENT_STATE);
    unsigned written = 0;
    uint64_t deadline = test_now + 5000000;
    while (written < total && test_now < deadline) {
        written += player_audio_client_write_pcm(input + written, total - written);
        if (written < total) vTaskDelay(5);
    }
    assert(written == total && player_audio_client_mark_eof() == 0);
    if (pause_once) {
        assert(player_audio_client_pause() == 0);
        player_audio_client_stats before, after;
        player_audio_client_get_status(&before);
        assert(before.state == PLAYER_AMP_AUDIO_PAUSED);
        vTaskDelay(50); player_audio_client_get_status(&after);
        assert(after.completed_samples == before.completed_samples);
        assert(player_audio_client_set_volume(7) == 0);
        assert(player_audio_client_resume() == 0);
    }
    player_audio_client_stats current;
    do {
        vTaskDelay(5); player_audio_client_get_status(&current);
        assert(current.last_error == 0 && current.completed_samples <= total);
    } while (current.state != PLAYER_AMP_AUDIO_DRAINED && test_now < deadline);
    assert(current.state == PLAYER_AMP_AUDIO_DRAINED && current.completed_samples == total);
    assert(player_audio_client_stop() == 0 && close_calls);
    player_audio_client_get_status(&current);
    assert(!current.session_active && !current.clock_valid && current.state == PLAYER_AMP_AUDIO_CLOSED);
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    assert(test_shared.pcm_read_pos == 0 && test_shared.pcm_write_pos == 0);
#endif
}

int main(void)
{
    reset_fixture();
    assert(player_audio_client_start(0, 10) == PLAYER_AUDIO_CLIENT_ARGUMENT);
    assert(player_audio_client_start(UINT32_MAX, 10) == PLAYER_AUDIO_CLIENT_ARGUMENT);
    assert(player_audio_client_start(1, 101) == PLAYER_AUDIO_CLIENT_ARGUMENT);
    stream(5003, 1);
    stream(31, 0);
    stream(PLAYER_AMP_PCM_SAMPLES + 3503u, 0);
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    hw_stats.dma_errors = 2; hw_stats.dma_timeouts = 3;
    hw_stats.queue_underruns = 5; hw_stats.fifo_underruns = 7;
    player_audio_client_stats counters;
    player_audio_client_get_status(&counters);
    assert(counters.dma_errors == 2 && counters.dma_timeouts == 3);
    assert(counters.queue_underruns == 5 && counters.fifo_underruns == 7 && counters.underruns == 12);
    assert(player_audio_client_stop() == 0);
    player_audio_client_get_status(&counters);
    assert(counters.dma_errors == 2 && counters.dma_timeouts == 3 && !counters.session_active);
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    command_sequence = rv_audio_command_sequence = UINT32_MAX;
    event_sequence = rv_audio_event_sequence = UINT32_MAX;
    assert(player_audio_client_set_volume(8) == 0 && command_sequence == 1 && event_sequence == 1);
    assert(player_audio_client_stop() == 0);
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    assert(rv_audio_halt() == 0 && !rv_audio_live && !test_shared.audio_session_active);
    assert(hw_stats.state == PLAYER_AUDIO_CLOSED);
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    status.written_samples = 4800;
    test_shared.audio_completed_frames = 1000;
    peer_blocked = 1;
    assert(player_audio_client_service() == 0 && status.completed_samples == 1000);
    assert(rv_audio_event(PLAYER_AMP_AUDIO_EVENT_PROGRESS, 480, 0, PLAYER_AMP_AUDIO_PLAYING, 1) == 1);
    assert(player_audio_client_service() == 0 && status.completed_samples == 1000);
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    stop_error_race = 1;
    assert(player_audio_client_stop() == 0 && !status.session_active);
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    test_shared.pcm_write_pos = test_shared.pcm_read_pos + PLAYER_AMP_PCM_SAMPLES + 1u;
    (void)player_audio_client_service();
    vTaskDelay(5);
    assert(player_audio_client_service() < 0 && status.state == PLAYER_AMP_AUDIO_FAULT);
    assert(player_audio_client_start(1, 10) == PLAYER_AUDIO_CLIENT_BUSY);
    assert(player_audio_client_stop() == 0 && !status.session_active);
    reset_fixture();
    peer_blocked = 1;
    uint64_t before = test_now;
    assert(player_audio_client_start(4800, 10) == PLAYER_AUDIO_CLIENT_TIMEOUT);
    assert(test_now - before <= 2005000 && status.session_active && protocol_broken);
    assert(force_silence_calls == 1 && client_recovery_required);
    assert(player_audio_client_stop() == PLAYER_AUDIO_CLIENT_TIMEOUT);
    assert(player_audio_client_start(1, 10) == PLAYER_AUDIO_CLIENT_BUSY);
    reset_fixture();
    wrong_ack_sequence = 1;
    assert(player_audio_client_start(4800, 10) == PLAYER_AUDIO_CLIENT_PROTOCOL && protocol_broken);
    reset_fixture();
    corrupt_event_crc = 1;
    assert(player_audio_client_start(4800, 10) == PLAYER_AUDIO_CLIENT_PROTOCOL && protocol_broken);
    reset_fixture();
    wrong_media_session = 1;
    assert(player_audio_client_start(4800, 10) == PLAYER_AUDIO_CLIENT_PROTOCOL && protocol_broken);
    reset_fixture();
    wrong_event_sequence = 1;
    assert(player_audio_client_start(4800, 10) == PLAYER_AUDIO_CLIENT_PROTOCOL && protocol_broken);
    reset_fixture();
    assert(player_audio_client_start(4800, 10) == 0);
    test_shared.pcm_read_pos = test_shared.pcm_write_pos = 17;
    bad_stop_ack = 1;
    assert(player_audio_client_stop() == PLAYER_AUDIO_CLIENT_PROTOCOL && status.session_active);
    assert(test_shared.pcm_read_pos == 17 && test_shared.pcm_write_pos == 17);
#endif
    printf("PASS production audio owner=%d: private-copy/sample-count/tail/pause/resume/volume/STOP/session/backpressure\n", PLAYER_AUDIO_OWNER);
#if PLAYER_AUDIO_OWNER == PLAYER_AUDIO_OWNER_RISCV
    printf("PASS production RV audio strict protocol/timeout/error race\n");
#endif
    return 0;
}
