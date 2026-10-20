#ifndef PLAYER_SESSION_PUBLIC_H
#define PLAYER_SESSION_PUBLIC_H

#include <stdint.h>

#define PLAYER_SESSION_PATH_BYTES 2048u
#define PLAYER_SESSION_LABEL_BYTES 1024u
#define PLAYER_SESSION_LIBRARY_WINDOW 10u
#define PLAYER_SESSION_COMMANDS 8u

enum player_session_state {
    PLAYER_SESSION_IDLE, PLAYER_SESSION_LOADING, PLAYER_SESSION_PLAYING,
    PLAYER_SESSION_PAUSED, PLAYER_SESSION_ENDED, PLAYER_SESSION_ERROR
};
enum player_session_command_kind {
    PLAYER_SESSION_PLAY_PATH = 1, PLAYER_SESSION_PAUSE, PLAYER_SESSION_RESUME,
    PLAYER_SESSION_TOGGLE_PAUSE, PLAYER_SESSION_STOP, PLAYER_SESSION_SET_VOLUME,
    PLAYER_SESSION_LIBRARY_OPEN, PLAYER_SESSION_LIBRARY_MOVE,
    PLAYER_SESSION_LIBRARY_SELECT, PLAYER_SESSION_LIBRARY_ACTIVATE,
    PLAYER_SESSION_LIBRARY_UP
};
typedef struct player_session_command {
    enum player_session_command_kind kind;
    uint32_t request_id; /* GUI-assigned, echoed in last_request_id. */
    uint32_t library_generation; /* 0=current, otherwise stale views rejected. */
    int32_t value; /* volume %, relative move, or absolute selected index.
                   * ACTIVATE -1 uses the worker's current selected index. */
    char path[PLAYER_SESSION_PATH_BYTES]; /* PLAY_PATH / LIBRARY_OPEN only. */
} player_session_command_t;

typedef struct player_session_entry {
    uint32_t index; /* Global filtered index, never a window-local index. */
    uint32_t token; /* library_generation; activate uses generation + index. */
    uint32_t is_directory;
    uint64_t bytes;
    char label[PLAYER_SESSION_LABEL_BYTES];
    char path[PLAYER_SESSION_PATH_BYTES];
} player_session_entry_t;

typedef struct player_session_snapshot {
    uint32_t revision;
    uint32_t session_id;
    enum player_session_state state;
    uint32_t media_kind; /* 0=none; otherwise enum player_media_kind. */
    uint32_t has_video;
    uint32_t volume_percent;
    uint32_t elapsed_samples; /* 48 kHz audio clock, valid PCM only. */
    uint32_t total_samples; /* Audio protocol total; zero until streaming EOF. */
    uint64_t duration_us; /* Edited media timeline, available on open; 0=unknown.
                           * Independent of the decoder's eventual PCM count. */
    uint32_t decoded_frames;
    uint32_t displayed_frames;
    uint32_t dropped_frames;
    uint32_t trimmed_frames; /* Decoded pictures outside the video edit window. */
    uint32_t audio_padding_samples; /* Timeline silence after the actual audio tail. */
    uint32_t weight_approximated_frames; /* AUs using the approved +128 -> +127 hardware fallback. */
    /* At actual presentation/drop decision, measured against the audio DMA
     * sample clock. Late means >=the source frame interval; not optical IRQ
     * latency. max_late_us saturates at UINT32_MAX. */
    uint32_t late_frames;
    uint32_t max_late_us;
    uint32_t video_queue_packets;
    uint32_t audio_queue_packets;
    uint32_t queued_pcm_samples;
    uint32_t audio_underruns;
    uint32_t last_request_id;
    int32_t last_request_result;
    int32_t error_code;
    uint32_t resources_pinned; /* Stop failed: no new session may reuse memory. */
    char error[160];
    char media_path[PLAYER_SESSION_PATH_BYTES];
    uint32_t library_generation;
    uint32_t library_scanning;
    uint32_t library_total_known;
    uint32_t library_total;
    uint32_t library_selected;
    uint32_t library_offset;
    uint32_t library_count;
    int32_t library_error;
    char library_path[PLAYER_SESSION_PATH_BYTES];
    player_session_entry_t entries[PLAYER_SESSION_LIBRARY_WINDOW];
} player_session_snapshot_t;

/* GUI calls only these nonblocking APIs. post copies the command and returns
 * 0, or -EAGAIN when its bounded queue is full; retry later. snapshot copies
 * one coherent view, no borrowed pointers. Storage/codec/display/audio belong
 * exclusively to the worker. A page is a ten-entry window over an unbounded
 * directory count; total_known becomes true after the incremental scan.
 */
int player_session_post(const player_session_command_t *command);
int player_session_snapshot(player_session_snapshot_t *snapshot);

#endif
