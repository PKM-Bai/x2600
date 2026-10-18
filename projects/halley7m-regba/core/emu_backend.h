/* Synchronous emulator/backend boundary. GPL-2.0-or-later. */
#ifndef X2600_EMU_BACKEND_H
#define X2600_EMU_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { EMU_PATH_CAPACITY = 512 };

typedef enum {
    /* Native GBA layout: R bits 0..4, G 5..9, B 10..14; bit 15 ignored. */
    EMU_PIXELS_RGB555 = 0,
} emu_pixel_format_t;

typedef struct {
    const uint16_t *pixels;
    unsigned width, height;
    unsigned stride_pixels;
    emu_pixel_format_t format;
} emu_video_frame_t;

/* Copied diagnostics; no guest pointers or engine-specific types escape.
 * Like the existing console status, this is not a cross-task transaction. */
typedef struct {
    bool loaded;
    char path[EMU_PATH_CAPACITY];
    uint32_t frames, drawn_frames, frame_crc;
    bool backup_dirty, save_in_flight, save_requested;
    uint32_t audio_max_call_gap_us, audio_gap_native, audio_gap_pending;
    uint32_t translation_depth, translation_peak;
} emu_backend_state_t;

/* Callbacks run synchronously in the emulator task, never in an ISR. The
 * host/context outlive the backend. Each blocking run has exactly one begin
 * and end, including each run performed by selftest().
 *
 * Video is borrowed only during video(); the host consumes/copies it before
 * returning. stride_pixels counts uint16_t pixels, not bytes. Audio is signed
 * 16-bit interleaved stereo; frames counts L/R pairs and sample_rate is Hz.
 * audio_submit() returns consumed frames (0..frames), owns no samples, and
 * must not retain the pointer. The backend preserves the unconsumed tail.
 *
 * frame() runs after the guest has committed its frame/IRQ/timer state. The
 * host services audio, saves and pacing there, returning true to stop. The
 * backend performs its own longjmp only after the callback has returned. */
typedef struct {
    void *context;
    uint16_t (*keys)(void *context);
    bool (*should_render)(void *context, uint32_t frame_index);
    void (*video)(void *context, const emu_video_frame_t *frame);
    size_t (*audio_submit)(void *context, const int16_t *samples,
                           size_t frames, uint32_t sample_rate);
    bool (*frame)(void *context, uint32_t completed_frames);
    void (*run_begin)(void *context, uint32_t frame_limit);
    /* Called after returning from native execution, before the final video
     * CRC. End runtime accounting and stop hardware audio here. */
    void (*run_quiesce)(void *context);
    /* reason retains the core diagnostic convention: 1=stop, 2=bad jump.
     * The final video CRC is now available for logging and save waits. */
    void (*run_end)(void *context, int reason);
    void (*load_progress)(void *context, unsigned percent);
    void (*stack_diagnostic)(void *context, const char *where);
} emu_backend_host_t;

/* These are backend operations, not the public asynchronous command facade.
 * All mutating calls belong to the single emulator task. run() blocks until
 * a frame-boundary stop or a core error; it does not reenter the JIT per frame.
 * The frame callback may call audio/save operations below on the same task.
 * No operation is called from an interrupt. Host callbacks are required.
 *
 * get_state(), diagnostics() and jit_test() retain the existing immediate
 * console diagnostic use; they do not mutate guest state or queue work. */
typedef struct {
    const char *name;
    int (*init)(const emu_backend_host_t *host);
    /* path is borrowed for this call only; a successful load owns its ROM
     * and file until a later successful switch/unload. Old save failure must
     * abort replacement and keep the current ROM and backup in memory. */
    int (*load)(const char *path);
    /* 0 frames means unlimited. Return 0 on normal stop, negative errno on
     * failure. Guest state remains available for a subsequent run(). */
    int (*run)(uint32_t frames);
    /* Loads the built-in diagnostic, runs 120 frames twice, and clears JIT
     * metadata between runs without resetting guest state. 0 means PASS. */
    int (*selftest)(void);
    /* The session first completes flush_save(); unload then frees the ROM. */
    void (*unload)(void);
    int (*check_rom)(const char *path);
    int (*request_save)(void);
    void (*service_save)(void);
    int (*flush_save)(void);
    /* Wait for an existing snapshot only: do not overwrite the old disk
     * save on menu entry, since the user may choose to reload it. */
    int (*settle_save)(void);
    int (*reload_save)(void);
    /* Before hardware start, discard pending/native PCM. Enable only once
     * hardware start succeeds; enabling also resets producer-gap timing. */
    void (*audio_reset)(void);
    void (*set_audio_enabled)(bool enabled);
    /* Drain native PCM after a frame-boundary hardware state change. Native
     * sound callbacks may invoke the same producer between frame boundaries. */
    int (*audio_update)(void);
    void (*get_state)(emu_backend_state_t *state);
    int (*jit_test)(void);
    /* Engine-specific diagnostic text, including the diagnostic save ROM
     * mailbox. No guest-memory address is returned to the session. */
    void (*diagnostics)(void);
} emu_backend_ops_t;

#endif
