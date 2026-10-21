#include "player_gui.h"
#include "player_gui_surface.h"
#include "player_session.h"
#include "lvgl.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static player_session_snapshot_t snapshot;
static player_session_command_t last_command;
static unsigned commands, moves, full_queue;
static uint32_t now;
static const uint32_t *front, *pending;
static uint32_t front_hash, pending_hash, pending_serial, due;
static unsigned submissions;
static const char *directory;
static unsigned mode_phase, mode_ack;
static uint32_t mode_generation;
static bool target_logical, memory_logical;
static void controls_visible(bool visible);

static uint32_t hash(const uint32_t *pixels)
{
    uint32_t value = 2166136261u;
    for (unsigned i = 0; i < PLAYER_GUI_SURFACE_BYTES / 4u; ++i)
        value = (value ^ pixels[i]) * 16777619u;
    return value;
}

int player_session_post(const player_session_command_t *command)
{
    if (full_queue) return -EAGAIN;
    last_command = *command;
    ++commands;
    if (command->kind == PLAYER_SESSION_LIBRARY_MOVE) {
        ++moves;
        snapshot.library_selected = (uint32_t)((int32_t)snapshot.library_selected + command->value);
        ++snapshot.revision;
    }
    if (command->kind == PLAYER_SESSION_SET_VOLUME) snapshot.volume_percent = (uint32_t)command->value;
    return 0;
}

int player_session_snapshot(player_session_snapshot_t *out)
{
    *out = snapshot;
    return 0;
}

static int submit(void *context, const uint32_t *argb, uint32_t bytes, uint32_t serial,
                  unsigned x1, unsigned y1, unsigned x2, unsigned y2)
{
    (void)context;
    assert(!pending && argb != front && bytes == PLAYER_GUI_SURFACE_BYTES && serial);
    assert(x1 <= x2 && y1 <= y2 && x2 < (memory_logical ? 1280u : 720u) &&
           y2 < (memory_logical ? 720u : 1280u));
    if (front) assert(front_hash == hash(front));
    pending = argb;
    pending_hash = hash(argb);
    pending_serial = serial;
    due = now + 40; /* Deliberately asynchronous, slower than a GUI loop. */
    ++submissions;
    return 0;
}

static int poll(void *context, uint32_t *serial)
{
    (void)context;
    assert(pending && pending_hash == hash(pending));
    if (front) assert(front_hash == hash(front));
    if ((int32_t)(now - due) < 0) return 0;
    front = pending;
    front_hash = pending_hash;
    pending = NULL;
    *serial = pending_serial;
    return 1;
}

static int output_mode(void *context, uint32_t *generation, bool *logical)
{ (void)context; *generation=mode_generation; *logical=target_logical; return (int)mode_phase; }
static int output_ack(void *context, uint32_t generation, unsigned phase)
{
    (void)context;
    assert(generation==mode_generation && phase==mode_phase && !pending);
    mode_ack=phase;
    if (phase==2) { memory_logical=target_logical; mode_phase=0; }
    return 0;
}

static void tick(unsigned duration, unsigned keys)
{
    for (unsigned i = 0; i < duration; i += 5) {
        now += 5;
        player_gui_sample_keys(now, (uint16_t)keys);
        int result = player_gui_step(now);
        if (result) fprintf(stderr, "GUI step failed at %u ms: %d\n", now, result);
        assert(result == 0);
    }
}

static void press(unsigned keys)
{
    tick(20, keys);
    tick(20, 0);
}

static void tap(unsigned x, unsigned y)
{
    player_gui_sample_touch(now, true, x, y);
    tick(60, 0);
    player_gui_sample_touch(now, false, x, y);
    tick(100, 0);
}

static void touch_library_test(void)
{
    player_gui_stats_t gui;
    unsigned before = commands;
    tap(600, 252); /* Third visible row uses its global index, not row 2. */
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_LIBRARY_ACTIVATE);
    assert(last_command.value == 23042 && last_command.library_generation == 1);
    before = commands;
    player_gui_sample_touch(now, true, 600, 156);
    tick(60, 0);
    snapshot.library_generation = 2;
    ++snapshot.revision;
    tick(150, 0);
    assert(commands == before); /* Directory refresh cannot synthesize a click. */
    player_gui_sample_touch(now, false, 600, 156);
    tick(100, 0);
    assert(commands == before);
    snapshot.library_generation = 1;
    ++snapshot.revision;
    tick(150, 0);
    player_gui_sample_touch(now, true, 600, 156);
    tick(60, 0);
    mode_phase = 1;
    mode_ack = 0;
    ++mode_generation;
    target_logical = false;
    tick(100, 0);
    assert(mode_ack == 1 && !pending);
    snapshot.library_generation = 2;
    ++snapshot.revision;
    player_gui_sample_touch(now, false, 600, 156);
    tick(100, 0); /* Physical release arrives while LVGL input is route-gated. */
    assert(commands == before);
    front = NULL;
    mode_phase = 2;
    tick(200, 0);
    assert(commands == before); /* Context cancellation must outlive sampled release. */
    snapshot.library_generation = 1;
    ++snapshot.revision;
    tick(150, 0);
    player_gui_sample_touch(now, true, 600, 156);
    tick(60, 0);
    snapshot.library_offset += 10;
    for (unsigned i = 0; i < 10; ++i) snapshot.entries[i].index += 10;
    ++snapshot.revision;
    tick(150, 0);
    player_gui_sample_touch(now, false, 600, 156);
    tick(100, 0);
    assert(commands == before); /* Key-driven paging keeps generation, changes row ownership. */
    snapshot.library_offset -= 10;
    for (unsigned i = 0; i < 10; ++i) snapshot.entries[i].index -= 10;
    ++snapshot.revision;
    tick(150, 0);
    before = commands;
    full_queue = 1;
    tap(600, 348);
    assert(commands == before);
    full_queue = 0;
    tick(150, 0);
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_LIBRARY_ACTIVATE);
    assert(last_command.value == 23044 && last_command.library_generation == 1);
    before = commands;
    player_gui_sample_touch(now, true, 600, 156);
    tick(60, 0);
    player_gui_sample_touch(now, true, 1270, 630);
    tick(60, 0);
    player_gui_sample_touch(now, false, 1270, 630);
    tick(100, 0);
    assert(commands == before); /* Dragging away cancels the original row click. */
    tap(120, 180);
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_UP);
    tap(120, 260);
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_MOVE && last_command.value == -10);
    tap(120, 338);
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_MOVE && last_command.value == 10);
    tap(120, 416);
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_OPEN);
    tap(1180, 68);
    player_gui_get_stats(&gui);
    assert(gui.menu_open && gui.menu_selected == 0);
    tap(620, 350); /* Menu's diagnostic item. */
    player_gui_get_stats(&gui);
    assert(gui.page == PLAYER_GUI_DIAGNOSTICS && !gui.menu_open);
    tap(1180, 68);
    player_gui_get_stats(&gui);
    assert(gui.page == PLAYER_GUI_LIBRARY);
}

static void touch_playback_test(void)
{
    player_gui_stats_t gui;
    snapshot.state = PLAYER_SESSION_PLAYING;
    snapshot.has_video = 1;
    snapshot.volume_percent = 7;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(false);
    unsigned before = commands;
    tap(524, 626); /* Hidden volume+ must wake without changing volume. */
    assert(commands == before && snapshot.volume_percent == 7);
    controls_visible(true);
    tap(524, 626);
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_SET_VOLUME && last_command.value == 12);
    tap(412, 626);
    assert(last_command.kind == PLAYER_SESSION_SET_VOLUME && last_command.value == 7);
    before = commands;
    player_gui_sample_touch(now, true, 524, 626);
    tick(60, 0);
    tick(850, PLAYER_INPUT_KEY_START);
    tick(100, 0);
    player_gui_get_stats(&gui);
    assert(gui.menu_open && gui.menu_selected == 0 && commands == before);
    player_gui_sample_touch(now, false, 524, 626);
    tick(100, 0);
    player_gui_get_stats(&gui);
    assert(gui.menu_selected == 0 && commands == before);
    tap(620, 430);
    snapshot.state = PLAYER_SESSION_ENDED;
    ++snapshot.revision;
    tick(150, 0);
    tap(240, 626);
    assert(last_command.kind == PLAYER_SESSION_PLAY_PATH);
    assert(!strcmp(last_command.path, snapshot.media_path));
    before = commands;
    press(PLAYER_INPUT_KEY_START);
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_PLAY_PATH);
    assert(!strcmp(last_command.path, snapshot.media_path));
    snapshot.state = PLAYER_SESSION_PLAYING;
    ++snapshot.revision;
    tick(150, 0);
    before = commands;
    player_gui_sample_touch(now, true, 524, 626);
    tick(1100, 0);
    assert(commands == before); /* A held touch never generates volume repeats. */
    player_gui_sample_touch(now, false, 524, 626);
    tick(150, 0);
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_SET_VOLUME && last_command.value == 12);
    tap(412, 626);
    assert(last_command.value == 7);
    tap(240, 626);
    assert(last_command.kind == PLAYER_SESSION_TOGGLE_PAUSE);
    before = commands;
    tap(600, 523); /* Progress is informational: tapping does not invent seek. */
    assert(commands == before);
    tap(980, 626);
    player_gui_get_stats(&gui);
    assert(gui.menu_open);
    tap(620, 430);
    player_gui_get_stats(&gui);
    assert(!gui.menu_open);
    tick(3300, 0);
    controls_visible(false);
}

static void capture(const char *name)
{
    tick(150, 0);
    assert(front);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.argb", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(front, 1, PLAYER_GUI_SURFACE_BYTES, file) == PLAYER_GUI_SURFACE_BYTES);
    assert(fclose(file) == 0);
}

static bool contains_label(lv_obj_t *object, const char *value)
{
    if (lv_obj_check_type(object,&lv_label_class) && strstr(lv_label_get_text(object),value)) return true;
    for (uint32_t i=0;i<lv_obj_get_child_cnt(object);++i)
        if (contains_label(lv_obj_get_child(object,(int32_t)i),value)) return true;
    return false;
}

static uint32_t pixel(unsigned x, unsigned y)
{
    assert(front);
    return front[memory_logical ? y * 1280u + x : x * 720u + 719u - y];
}

static void controls_visible(bool visible)
{
    player_gui_stats_t gui;
    player_gui_get_stats(&gui);
    assert((gui.controls_visible != 0) == visible);
    /* Check the actually scanned, completed front, including when a newer
     * volume/pressed-state repaint is pending in the other DMA surface. */
    assert(front);
    if (visible) {
        if (gui.menu_open) {
            assert(pixel(1230, 100) >> 24); /* Modal scrim dims the visible controls. */
            assert(pixel(640, 180) >> 24 == 255);
        } else if (!snapshot.has_video) {
            assert(pixel(1230, 100) >> 24 == 255);
            assert(pixel(640, 400) >> 24 == 255); /* Audio owns an opaque canvas. */
        } else {
            assert(pixel(1230, 100) == 0xdc111e2a);
            assert(pixel(1240, 710) == 0xbe0b121b);
            assert(pixel(150, 524) >> 24 == 255); /* Progress remains visible. */
        }
    } else {
        for (unsigned i = 0; i < PLAYER_GUI_SURFACE_BYTES / 4u; ++i)
            assert(front[i] >> 24 == 0); /* No title, footer, progress or ghost pixels. */
    }
}

static void video_controls_test(void)
{
    player_gui_stats_t gui;
    snapshot.has_video = 1;
    snapshot.state = PLAYER_SESSION_PLAYING;
    snapshot.duration_us = 180000000u;
    snapshot.total_samples = 0; /* MP4 keeps the final PCM count unknown until EOF. */
    ++snapshot.revision;
    press(PLAYER_INPUT_KEY_A);
    tick(150, 0);
    assert(contains_label(lv_scr_act(), "00:37 / 03:00"));
    controls_visible(true);
    tick(2800, 0);
    controls_visible(true); /* Not hidden before the three-second timeout. */
    tick(250, 0);
    controls_visible(false);
    unsigned before = commands;
    full_queue = 1;
    press(PLAYER_INPUT_KEY_B);
    tick(150, 0);
    assert(commands == before);
    controls_visible(true); /* Command queue backpressure must not delay wake-up. */
    full_queue = 0;
    tick(150, 0);
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_SET_VOLUME);
    controls_visible(true); /* Wake also executes the original command. */
    tick(3100, 0);
    controls_visible(false); /* Repeat the full clear after swapping both surfaces. */
    before = commands;
    tick(60, PLAYER_INPUT_KEY_START);
    assert(commands == before); /* Short-press command still waits for release. */
    controls_visible(true); /* The press itself already woke the overlay. */
    tick(150, 0);
    assert(commands == before + 1 && last_command.kind == PLAYER_SESSION_TOGGLE_PAUSE);
    snapshot.state = PLAYER_SESSION_PAUSED;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(true);
    snapshot.state = PLAYER_SESSION_PLAYING;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(false);
    before = commands;
    tick(850, PLAYER_INPUT_KEY_START);
    tick(150, 0);
    player_gui_get_stats(&gui);
    assert(gui.menu_open && commands == before);
    tick(3300, 0);
    controls_visible(true); /* Menu remains usable over an otherwise idle video. */
    tick(850, PLAYER_INPUT_KEY_START);
    tick(150, 0);
    player_gui_get_stats(&gui);
    assert(!gui.menu_open && commands == before);
    tick(3100, 0);
    controls_visible(false);
    snapshot.state = PLAYER_SESSION_ENDED;
    snapshot.elapsed_samples = 180u * 48000u - 1000u;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(true);
    assert(pixel(1138, 524) == 0xff65dfc4); /* EOF fills the track despite AAC trimming. */
    snapshot.state = PLAYER_SESSION_PLAYING;
    snapshot.has_video = 0;
    snapshot.media_kind = 3;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(true); /* Audio playback does not disappear. */
    snapshot.has_video = 1;
    snapshot.media_kind = 2;
    snapshot.elapsed_samples = 48000u * 37u;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(false);
    snapshot.error_code = -EIO;
    strcpy(snapshot.error, "测试播放错误");
    ++snapshot.revision;
    tick(150, 0);
    controls_visible(true);
    assert(contains_label(lv_scr_act(), snapshot.error));
    snapshot.error_code = 0;
    snapshot.error[0] = 0;
    snapshot.resources_pinned = 1;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(true);
    assert(contains_label(lv_scr_act(), "停止尚未完成"));
    snapshot.resources_pinned = 0;
    ++snapshot.revision;
    tick(3300, 0);
    controls_visible(false);
    /* An uninterrupted held key, including its repeats, keeps the UI awake. */
    tick(3300, PLAYER_INPUT_KEY_A);
    tick(150, 0);
    controls_visible(true);
}

static void surface_test(void)
{
    uint32_t *a = malloc(PLAYER_GUI_SURFACE_BYTES + 8u);
    uint32_t *b = malloc(PLAYER_GUI_SURFACE_BYTES + 8u);
    assert(a && b);
    a[0] = b[0] = 0x12345678;
    a[PLAYER_GUI_SURFACE_BYTES / 4u + 1] = b[PLAYER_GUI_SURFACE_BYTES / 4u + 1] = 0x87654321;
    player_gui_surface_t s;
    player_gui_surface_init(&s, a + 1, b + 1);
    const uint32_t pixels[] = {0x80112233, 0x40112233, 0xff445566, 0x00667788};
    assert(player_gui_surface_write(&s, 9, 19, 10, 20, pixels));
    assert(s.pixels[0][9 * 720 + 719 - 19] == pixels[0]);
    assert(s.pixels[0][10 * 720 + 719 - 20] == pixels[3]);
    assert(s.pixels[1][9 * 720 + 719 - 19] == 0);
    assert(player_gui_surface_seal(&s));
    assert(!player_gui_surface_write(&s, 0, 0, 0, 0, pixels));
    assert(player_gui_surface_submitted(&s));
    assert(!player_gui_surface_reconfigure(&s,true));
    assert(!player_gui_surface_complete(&s, s.serial + 1));
    assert(player_gui_surface_complete(&s, s.serial));
    assert(s.front == 0 && s.back == 1);
    assert(memcmp(s.pixels[0], s.pixels[1], PLAYER_GUI_SURFACE_BYTES) == 0);
    assert(!player_gui_surface_complete(&s, s.serial));
    assert(player_gui_surface_write(&s, 90, 190, 90, 190, pixels));
    assert(s.publish_x1 == 529 && s.publish_x2 == 700);
    assert(s.publish_y1 == 9 && s.publish_y2 == 90);
    assert(!player_gui_surface_write(&s, -1, 0, 0, 0, pixels));
    assert(!player_gui_surface_write(&s, 0, 0, 1280, 0, pixels));
    assert(a[0] == 0x12345678 && b[0] == 0x12345678);
    assert(a[PLAYER_GUI_SURFACE_BYTES / 4u + 1] == 0x87654321);
    assert(b[PLAYER_GUI_SURFACE_BYTES / 4u + 1] == 0x87654321);
    assert(player_gui_surface_reconfigure(&s,true) && s.logical);
    assert(player_gui_surface_write(&s,9,19,10,20,pixels));
    assert(s.pixels[0][19*1280+9]==pixels[0] && s.pixels[0][20*1280+10]==pixels[3]);
    assert(player_gui_surface_seal(&s) && player_gui_surface_submitted(&s));
    assert(player_gui_surface_complete(&s,s.serial));
    assert(memcmp(s.pixels[0],s.pixels[1],PLAYER_GUI_SURFACE_BYTES)==0);
    assert(a[0]==0x12345678 && b[PLAYER_GUI_SURFACE_BYTES/4u+1]==0x87654321);
    free(a); free(b);
}

static void font_test(void)
{
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(&player_font_32, &glyph, 0x5a92, 0));
    assert(glyph.box_w == 32 && glyph.box_h == 32);
    assert(lv_font_get_glyph_dsc(&player_font_32, &glyph, 'A', 0));
    assert(glyph.box_w == 16);
    const uint8_t *bits = lv_font_get_glyph_bitmap(&player_font_32, 'A');
    for (unsigned y = 0; y < 32; y += 2) {
        assert(bits[y * 2] == bits[(y + 1) * 2]);
        assert(bits[y * 2 + 1] == bits[(y + 1) * 2 + 1]);
        for (unsigned x = 0; x < 16; x += 2) {
            unsigned byte = y * 2 + x / 8;
            unsigned shift = 6u - x % 8u;
            unsigned pair = (bits[byte] >> shift) & 3u;
            assert(pair == 0 || pair == 3);
        }
    }
    assert(lv_font_get_glyph_dsc(&player_font_32, &glyph, 0x20000, 0));
    assert(glyph.box_w == 32 && lv_font_get_glyph_bitmap(&player_font_32, 0x20000)[0] == 0xff);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    directory = argv[1];
    surface_test();
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.revision = snapshot.library_generation = 1;
    snapshot.library_total = 54321;
    snapshot.library_total_known = 1;
    snapshot.library_selected = snapshot.library_offset = 23040;
    snapshot.library_count = 10;
    snapshot.volume_percent = 65;
    strcpy(snapshot.library_path, "0:/本地媒体/旅行记录");
    for (unsigned i = 0; i < 10; ++i) {
        snapshot.entries[i].index = 23040 + i;
        snapshot.entries[i].token = 1;
        snapshot.entries[i].is_directory = i == 0;
        snprintf(snapshot.entries[i].label, sizeof(snapshot.entries[i].label),
                 i == 0 ? ".." : "%02u - 清晨的城市与海边漫步.avi", i);
    }
    player_gui_output_t backend = {NULL, submit, poll, output_mode, output_ack, NULL};
    assert(player_gui_init(&backend) == 0);
    font_test();
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_OPEN);
    capture("library");
    touch_library_test();
    unsigned queued_commands = commands;
    now += 5;
    player_gui_sample_keys(now, PLAYER_INPUT_KEY_B);
    now += 40;
    player_gui_sample_keys(now, 0); /* Full press/release while GUI was busy. */
    assert(player_gui_step(now) == 0 && commands == queued_commands + 1);
    press(PLAYER_INPUT_KEY_B);
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_MOVE && last_command.value == 1);
    tick(600, PLAYER_INPUT_KEY_B);
    tick(20, 0);
    assert(moves >= 4); /* Immediate and 350/450/550 ms repeats. */
    full_queue = 1;
    unsigned old_commands = commands;
    press(PLAYER_INPUT_KEY_A);
    assert(commands == old_commands);
    full_queue = 0;
    tick(10, 0);
    assert(commands == old_commands + 1);
    press(PLAYER_INPUT_KEY_START);
    assert(last_command.kind == PLAYER_SESSION_LIBRARY_ACTIVATE);
    assert(last_command.value == -1 && last_command.library_generation == 1);
    snapshot.session_id = 1;
    snapshot.state = PLAYER_SESSION_PLAYING;
    snapshot.media_kind = 2;
    snapshot.has_video = 1;
    snapshot.elapsed_samples = 48000 * 37;
    snapshot.total_samples = 48000 * 180;
    snapshot.duration_us = 180000000u;
    snapshot.decoded_frames = 1110;
    snapshot.displayed_frames = 1108;
    snapshot.weight_approximated_frames = 5;
    strcpy(snapshot.media_path, "0:/本地媒体/清晨的城市与海边漫步.avi");
    ++snapshot.revision;
    capture("playback");
    assert(front[500 * 720 + 719 - 250] == 0); /* Center of video remains transparent. */
    assert(pixel(1230, 100) == 0xdc111e2a); /* Real LVGL output is straight, not premultiplied. */
    press(PLAYER_INPUT_KEY_START);
    assert(last_command.kind == PLAYER_SESSION_TOGGLE_PAUSE);
    press(PLAYER_INPUT_KEY_A);
    assert(last_command.kind == PLAYER_SESSION_SET_VOLUME && last_command.value == 60);
    press(PLAYER_INPUT_KEY_A);
    assert(last_command.kind == PLAYER_SESSION_SET_VOLUME && last_command.value == 55);
    unsigned before_long = commands;
    tick(850, PLAYER_INPUT_KEY_START);
    tick(20, 0);
    player_gui_stats_t gui;
    player_gui_get_stats(&gui);
    assert(gui.menu_open && commands == before_long); /* Long release produces no short action. */
    capture("menu");
    press(PLAYER_INPUT_KEY_B);
    press(PLAYER_INPUT_KEY_START);
    player_gui_get_stats(&gui);
    assert(gui.page == PLAYER_GUI_DIAGNOSTICS && !gui.menu_open);
    capture("diagnostics");
    assert(contains_label(lv_scr_act(), "权重近似：5帧"));
    press(PLAYER_INPUT_KEY_START);
    player_gui_get_stats(&gui);
    assert(gui.page == PLAYER_GUI_PLAYBACK);
    assert(gui.presented > 4 && gui.output_error == 0 && submissions >= gui.presented);
    snapshot.media_kind=3; snapshot.has_video=0; snapshot.total_samples=0;
    snapshot.duration_us=0; ++snapshot.revision;
    capture("audio-unknown");
    assert(contains_label(lv_scr_act(),"--:--") && contains_label(lv_scr_act(),"MP3音频"));
    snapshot.media_kind=4; ++snapshot.revision; tick(150,0);
    assert(contains_label(lv_scr_act(),"AAC音频"));
    snapshot.media_kind=5; strcpy(snapshot.media_path,"0:/音乐/短曲.M4A");
    ++snapshot.revision; tick(150,0);
    assert(contains_label(lv_scr_act(),"M4A音频"));
    snapshot.total_samples=snapshot.elapsed_samples=96017;
    snapshot.duration_us=(uint64_t)snapshot.total_samples*1000000u/48000u;
    snapshot.state=PLAYER_SESSION_ENDED; ++snapshot.revision; tick(150,0);
    assert(!contains_label(lv_scr_act(),"--:--") && contains_label(lv_scr_act(),"00:02 / 00:02"));
    snapshot.media_kind=2; snapshot.state=PLAYER_SESSION_PLAYING;
    snapshot.has_video=1; snapshot.duration_us=180000000u;
    snapshot.elapsed_samples=48000*37; snapshot.total_samples=48000*180;
    strcpy(snapshot.media_path,"0:/本地媒体/清晨的城市与海边漫步.avi"); ++snapshot.revision;
    tick(150,0);
    mode_phase=1; mode_ack=0; ++mode_generation; target_logical=true;
    unsigned before_mode=submissions;
    tick(100,0);
    assert(mode_ack==1 && !pending && submissions==before_mode);
    assert(front_hash==hash(front)); /* Pause must not overwrite the scanned old layout. */
    front=NULL; /* Driver reports phase2 only after the old DMA has stopped. */
    mode_phase=2;
    capture("playback-wide");
    player_gui_get_stats(&gui);
    assert(mode_ack==2 && gui.logical_output && gui.output_generation==mode_generation);
    assert(front[250*1280+500]==0 && pixel(1230, 100)==0xdc111e2a);
    touch_playback_test();
    video_controls_test();
    mode_phase=1; mode_ack=0; ++mode_generation; target_logical=false;
    tick(100,0);
    assert(mode_ack==1 && !pending);
    front=NULL; mode_phase=2; tick(150,0);
    player_gui_get_stats(&gui);
    assert(!gui.logical_output && gui.output_generation==mode_generation && pixel(1230, 100)==0xdc111e2a);
    touch_playback_test();
    video_controls_test();
    /* The activity deadline must also survive the 32-bit millisecond wrap. */
    now = UINT32_MAX - 1500u;
    tick(150, PLAYER_INPUT_KEY_A);
    tick(100, 0);
    controls_visible(true);
    tick(2700, 0);
    controls_visible(true);
    tick(400, 0);
    controls_visible(false);
    press(PLAYER_INPUT_KEY_B);
    tick(150, 0);
    controls_visible(true);
    tap(704, 626);
    player_gui_get_stats(&gui);
    assert(last_command.kind == PLAYER_SESSION_STOP && gui.page == PLAYER_GUI_LIBRARY);
    assert(player_gui_deinit(0) == -EBUSY);
    assert(player_gui_deinit(1) == 0);
    printf("PASS production LVGL GUI: native/logical rotation/guards, async pin/sync, GUI route drain/rebuild ACK, font, 3 keys, pointer click/release/drag/global-index/backpressure, touch wake-only/volume/pause/stop/menu, video overlay idle/wake/alpha/wrap, duration metadata/EOF fill, three pages/menu (%u submissions)\n", submissions);
    return 0;
}
