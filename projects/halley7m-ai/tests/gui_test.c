#include "ai_gui.h"
#include "ai_history.h"
#include "player_gui_surface.h"
#include "lvgl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    const uint32_t *pending, *front, *rejected, *addresses[2];
    uint32_t *pending_copy, *front_copy;
    uint32_t serial, last_serial, submitted, completed, rejects, retry_attempts;
    unsigned address_count, polls_left;
    int hold, attached;
} memory_output;

static memory_output backend;
static uint32_t now_ms;
static unsigned send_count;
static int send_result = -ENETDOWN;
static char sent[AI_INPUT_BYTES + 1];

static void check_pinned(void)
{
    if (backend.front) CHECK(!memcmp(backend.front, backend.front_copy, PLAYER_GUI_SURFACE_BYTES));
    if (backend.pending) CHECK(!memcmp(backend.pending, backend.pending_copy, PLAYER_GUI_SURFACE_BYTES));
    if (backend.rejected) CHECK(!memcmp(backend.rejected, backend.pending_copy, PLAYER_GUI_SURFACE_BYTES));
}

static int submit(void *context, const uint32_t *argb, uint32_t bytes, uint32_t serial,
                  unsigned x1, unsigned y1, unsigned x2, unsigned y2)
{
    memory_output *b = context;
    CHECK(bytes == PLAYER_GUI_SURFACE_BYTES && !b->pending && serial);
    CHECK(x1 <= x2 && x2 < PLAYER_GUI_NATIVE_WIDTH && y1 <= y2 && y2 < PLAYER_GUI_NATIVE_HEIGHT);
    CHECK(argb != b->front);
    check_pinned();
    if (b->rejected) CHECK(b->rejected == argb && b->serial == serial);
    else {
        CHECK(serial != b->last_serial);
        memcpy(b->pending_copy, argb, bytes);
        b->serial = serial;
    }
    if (b->rejects) {
        --b->rejects; ++b->retry_attempts; b->rejected = argb;
        return -EAGAIN;
    }
    b->rejected = NULL;
    b->pending = argb;
    b->last_serial = serial;
    b->polls_left = 2;
    ++b->submitted;
    unsigned i;
    for (i = 0; i < b->address_count; ++i) if (b->addresses[i] == argb) break;
    if (i == b->address_count) {
        CHECK(b->address_count < 2);
        b->addresses[b->address_count++] = argb;
    }
    return 0;
}

static int poll(void *context, uint32_t *serial)
{
    memory_output *b = context;
    check_pinned();
    if (!b->pending || b->hold) return 0;
    if (b->polls_left) { --b->polls_left; return 0; }
    *serial = b->serial;
    b->front = b->pending;
    memcpy(b->front_copy, b->pending_copy, PLAYER_GUI_SURFACE_BYTES);
    b->pending = NULL;
    ++b->completed;
    return 1;
}

static void attach(void *context, bool attached)
{ ((memory_output *)context)->attached = attached; }

static int send_text(void *context, const char *text)
{
    CHECK(context == &send_count && strlen(text) <= AI_INPUT_BYTES);
    ++send_count;
    strcpy(sent, text);
    return send_result;
}

static void step(bool pressed, unsigned x, unsigned y)
{
    check_pinned();
    now_ms += 5;
    CHECK(ai_gui_step(now_ms, pressed, x, y, 0) == 0);
    check_pinned();
}

static void settle(unsigned steps)
{ for (unsigned i = 0; i < steps; ++i) step(false, 0, 0); }

static lv_obj_t *find_class(lv_obj_t *parent, const lv_obj_class_t *type, unsigned *skip)
{
    if (lv_obj_check_type(parent, type)) {
        if (!*skip) return parent;
        --*skip;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
        lv_obj_t *found = find_class(lv_obj_get_child(parent, (int32_t)i), type, skip);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_caption(lv_obj_t *parent, const char *caption)
{
    if (lv_obj_check_type(parent, &lv_label_class) && !strcmp(lv_label_get_text(parent), caption)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
        lv_obj_t *found = find_caption(lv_obj_get_child(parent, (int32_t)i), caption);
        if (found) return found;
    }
    return NULL;
}

static void click(lv_obj_t *object)
{
    CHECK(object);
    lv_obj_update_layout(object);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    CHECK(area.x1 >= 0 && area.x2 < 1280 && area.y1 >= 0 && area.y2 < 720);
    unsigned x = (unsigned)(area.x1 + area.x2) / 2;
    unsigned y = (unsigned)(area.y1 + area.y2) / 2;
    for (unsigned i = 0; i < 8; ++i) step(true, x, y);
    for (unsigned i = 0; i < 8; ++i) step(false, x, y);
}

static void click_caption(const char *caption)
{
    lv_obj_t *text = find_caption(lv_scr_act(), caption);
    CHECK(text && lv_obj_check_type(lv_obj_get_parent(text), &lv_btn_class));
    click(lv_obj_get_parent(text));
}

static void keyboard_key(lv_obj_t *keyboard, const char *caption)
{
    for (uint16_t i = 0; i < 100; ++i) {
        const char *text = lv_btnmatrix_get_btn_text(keyboard, i);
        if (!text) break;
        if (!strcmp(text, caption)) {
            lv_btnmatrix_set_selected_btn(keyboard, i);
            CHECK(lv_event_send(keyboard, LV_EVENT_VALUE_CHANGED, NULL) == LV_RES_OK);
            return;
        }
    }
    CHECK(0 && "keyboard caption missing");
}

static void check_caption_bounds(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class) &&
        lv_obj_check_type(lv_obj_get_parent(object), &lv_btn_class)) {
        lv_area_t text, button;
        lv_obj_get_coords(object, &text);
        lv_obj_get_coords(lv_obj_get_parent(object), &button);
        CHECK(text.x1 >= button.x1 && text.x2 <= button.x2);
        CHECK(text.y1 >= button.y1 && text.y2 <= button.y2);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
        check_caption_bounds(lv_obj_get_child(object, (int32_t)i));
}

static void screenshot(const char *directory, const char *name)
{
    CHECK(backend.front);
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.ppm", directory, name) > 0);
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fprintf(file, "P6\n1280 720\n255\n") > 0);
    for (unsigned y = 0; y < 720; ++y) for (unsigned x = 0; x < 1280; ++x) {
        uint32_t color = backend.front_copy[x * 720 + 719 - y];
        uint8_t rgb[3] = {(uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color};
        CHECK(fwrite(rgb, 1, 3, file) == 3);
    }
    CHECK(fclose(file) == 0);
}

static uint8_t *load_asset(const char *path, const char *magic, ai_asset *asset)
{
    FILE *file = fopen(path, "rb"); CHECK(file);
    CHECK(!fseek(file, 0, SEEK_END)); long bytes = ftell(file); CHECK(bytes > 16);
    rewind(file);
    uint8_t *data = malloc((size_t)bytes); CHECK(data);
    CHECK(fread(data, 1, (size_t)bytes, file) == (size_t)bytes);
    CHECK(!fclose(file) && !ai_asset_open(asset, data, (size_t)bytes, magic));
    return data;
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
#ifdef _WIN32
    CHECK(_mkdir(argv[3]) == 0 || errno == EEXIST);
#else
    CHECK(mkdir(argv[3], 0777) == 0 || errno == EEXIST);
#endif
    ai_asset font, dict;
    uint8_t *font_bytes = load_asset(argv[1], "AIF1", &font);
    uint8_t *dict_bytes = load_asset(argv[2], "AIP1", &dict);
    CHECK(!ai_font_validate(&font) && !ai_dictionary_validate(&dict));
    backend.pending_copy = malloc(PLAYER_GUI_SURFACE_BYTES);
    backend.front_copy = malloc(PLAYER_GUI_SURFACE_BYTES);
    CHECK(backend.pending_copy && backend.front_copy);
    player_gui_output_t out = {0};
    out.context = &backend; out.submit = submit; out.poll = poll; out.attach = attach;
    CHECK(!ai_gui_init(&out, &font, &dict, "尚未连接网络 · 输入会保留在本机", send_text, &send_count));
    settle(60);
    CHECK(backend.attached && backend.completed);
    screenshot(argv[3], "01-initial");
    unsigned skip = 0;
    lv_obj_t *editor = find_class(lv_scr_act(), &lv_textarea_class, &skip);
    skip = 1;
    lv_obj_t *composition = find_class(lv_scr_act(), &lv_textarea_class, &skip);
    skip = 0;
    lv_obj_t *keyboard = find_class(lv_scr_act(), &lv_keyboard_class, &skip);
    CHECK(editor && composition && keyboard && !*ai_gui_draft());
    lv_area_t keyboard_area;
    lv_obj_get_coords(keyboard, &keyboard_area);
    printf("Keyboard coordinates: %d,%d..%d,%d\n", keyboard_area.x1, keyboard_area.y1,
           keyboard_area.x2, keyboard_area.y2);
    CHECK(keyboard_area.x1 == 24 && keyboard_area.y1 == 530 && keyboard_area.x2 == 1253 && keyboard_area.y2 == 709);
    check_caption_bounds(lv_scr_act());
    CHECK(lv_keyboard_get_textarea(keyboard) == composition);
    CHECK(lv_obj_get_style_bg_opa(composition, 0) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_border_width(editor, LV_PART_CURSOR) == 2);
    CHECK(lv_obj_get_style_border_opa(editor, LV_PART_CURSOR) == LV_OPA_TRANSP);
    CHECK(lv_obj_get_style_border_opa(composition, LV_PART_CURSOR) == LV_OPA_COVER);
    lv_textarea_set_text(composition, "nihao");
    settle(40);
    screenshot(argv[3], "02-pinyin-candidates");
    click_caption("你好");
    CHECK(!strcmp(ai_gui_draft(), "你好") && !*lv_textarea_get_text(composition));
    click_caption("中文 / EN");
    CHECK(lv_keyboard_get_textarea(keyboard) == editor);
    CHECK(lv_obj_get_style_border_opa(editor, LV_PART_CURSOR) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_border_opa(composition, LV_PART_CURSOR) == LV_OPA_TRANSP);
    keyboard_key(keyboard, "a");
    CHECK(!strcmp(ai_gui_draft(), "你好a"));
    keyboard_key(keyboard, LV_SYMBOL_BACKSPACE);
    CHECK(!strcmp(ai_gui_draft(), "你好"));
    lv_textarea_add_text(editor, " AI 123!");
    CHECK(!strcmp(ai_gui_draft(), "你好 AI 123!"));
    click_caption("发送");
    CHECK(send_count == 1 && !strcmp(sent, "你好 AI 123!") && !strcmp(ai_gui_draft(), sent));
    CHECK(find_caption(lv_scr_act(), "尚未连接云端，输入已保留"));
    settle(40); screenshot(argv[3], "03-send-failed");

    /* Hold a real submitted surface through edits and the 400 ms cursor blink. */
    backend.hold = 1;
    ai_gui_set_status("等待显示完成");
    for (unsigned i = 0; i < 100 && !backend.pending; ++i) step(false, 0, 0);
    CHECK(backend.pending);
    uint32_t submissions = backend.submitted;
    lv_textarea_set_text(editor, "持有前台时仍可编辑");
    settle(120);
    CHECK(backend.submitted == submissions);
    backend.hold = 0;
    settle(60);
    CHECK(backend.address_count == 2 && !strcmp(ai_gui_draft(), "持有前台时仍可编辑"));
    backend.rejects = 2;
    ai_gui_set_status("显示短暂繁忙后恢复");
    settle(60);
    CHECK(backend.rejects == 0 && backend.retry_attempts == 2 && !backend.rejected);

    click_caption("新会话");
    CHECK(!*ai_gui_draft() && !*lv_textarea_get_text(composition));
    CHECK(find_caption(lv_scr_act(), "新会话：本地草稿已清空"));
    lv_textarea_set_text(editor, "成功提交");
    send_result = 0;
    click_caption("发送");
    CHECK(send_count == 2 && !strcmp(sent, "成功提交") && !*ai_gui_draft());
    settle(40); screenshot(argv[3], "04-cleared");
    check_caption_bounds(lv_scr_act());
    printf("PASS real LVGL: Chinese candidates/EN/edit/send preservation/new draft, two surfaces, delayed IRQ and EAGAIN; submit=%u complete=%u\n",
           backend.submitted, backend.completed);
    /* GUI is a process-lifetime singleton; it has no deinit API. These resource
     * bytes remain borrowed until process exit, just like the board resources. */
    (void)font_bytes; (void)dict_bytes;
    return 0;
}
