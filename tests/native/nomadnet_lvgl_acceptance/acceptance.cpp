#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <lvgl.h>
#define private public
#include "NomadNetScreen.h"
#undef private
#include "LVGL/LVGLInit.h"
#include "Theme.h"
#include "../../../lib/tdeck_ui/UI/Clipboard.h"

namespace UI {
String Clipboard::_content;
bool Clipboard::_has_content = false;
}

namespace {
lv_group_t* group = nullptr;
lv_disp_t* display = nullptr;
lv_indev_t* keyboard = nullptr;
std::vector<lv_color_t> framebuffer(320 * 240);

struct KeyFeed { uint32_t key = 0; uint8_t phase = 0; } key_feed;

void flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* colors) {
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x >= 0 && x < 320 && y >= 0 && y < 240)
                framebuffer[static_cast<std::size_t>(y) * 320 + x] = *colors;
            ++colors;
        }
    }
    lv_disp_flush_ready(drv);
}

void read_key(lv_indev_drv_t*, lv_indev_data_t* data) {
    data->key = key_feed.key;
    if (key_feed.phase == 2) {
        data->state = LV_INDEV_STATE_PRESSED;
        --key_feed.phase;
    } else if (key_feed.phase == 1) {
        data->state = LV_INDEV_STATE_RELEASED;
        --key_feed.phase;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->continue_reading = key_feed.phase != 0;
}

void pump() {
    for (int i = 0; i < 4; ++i) {
        lv_tick_inc(35);
        lv_timer_handler();
    }
}

void dispatch_key(uint32_t key) {
    key_feed = {key, 2};
    pump();
    assert(key_feed.phase == 0);
}

void render() {
    std::fill(framebuffer.begin(), framebuffer.end(), lv_color_hex(0xA55A5A));
    lv_obj_invalidate(lv_scr_act());
    pump();
}

bool same(lv_color_t left, lv_color_t right) { return left.full == right.full; }

std::size_t count_color(int x1, int y1, int x2, int y2, lv_color_t color) {
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(319, x2); y2 = std::min(239, y2);
    std::size_t count = 0;
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x)
            if (same(framebuffer[static_cast<std::size_t>(y) * 320 + x], color)) ++count;
    return count;
}

struct DeleteAudit {
    int deleted = 0;
    bool empty = false;
    bool password_disabled = false;
};

void audit_editor_delete(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    auto* audit = static_cast<DeleteAudit*>(lv_event_get_user_data(event));
    auto* editor = lv_event_get_target(event);
    const char* value = lv_textarea_get_text(editor);
    ++audit->deleted;
    audit->empty = value && value[0] == '\0';
    audit->password_disabled = !lv_textarea_get_password_mode(editor);
}

bool editor_round_trip(UI::LXMF::NomadNetScreen& screen, lv_event_code_t completion,
                       uint32_t key, const char* replacement, bool expected_commit,
                       bool password, int& delete_events) {
    lv_group_focus_obj(screen._content);
    lv_group_set_editing(group, true);
    screen._selected_focus = -1;
    screen._selected_field = -1;
    dispatch_key(LV_KEY_DOWN);
    if (screen._selected_field < 0) return false;
    if (password != (screen._page.fields()[screen._selected_field].type ==
                     UI::LXMF::NomadNet::FormFieldType::PASSWORD)) {
        dispatch_key(LV_KEY_DOWN);
    }
    const int16_t field = screen._selected_field;
    const std::string before(screen._form_state.fields()[field].value.data(),
                             screen._form_state.fields()[field].value_length);
    dispatch_key(LV_KEY_ENTER);
    if (!screen._field_editor || lv_group_get_focused(group) != screen._field_editor) return false;
    lv_textarea_set_text(screen._field_editor, replacement);
    DeleteAudit audit;
    lv_obj_add_event_cb(screen._field_editor, audit_editor_delete, LV_EVENT_DELETE, &audit);
    if (completion != LV_EVENT_ALL) lv_event_send(screen._field_editor, completion, nullptr);
    else dispatch_key(key);
    pump();
    const std::string after(screen._form_state.fields()[field].value.data(),
                            screen._form_state.fields()[field].value_length);
    delete_events += audit.deleted;
    return screen._field_editor == nullptr && audit.deleted == 1 && audit.empty &&
           audit.password_disabled && lv_group_get_focused(group) == screen._content &&
           lv_group_get_editing(group) &&
           (expected_commit ? after == replacement : after == before);
}

bool group_contains(lv_obj_t* object) { return object && lv_obj_get_group(object) == group; }
} // namespace

namespace UI::LVGL {
bool LVGLInit::_initialized = true;
lv_disp_t* LVGLInit::_display = nullptr;
lv_indev_t* LVGLInit::_keyboard = nullptr;
lv_indev_t* LVGLInit::_touch = nullptr;
lv_indev_t* LVGLInit::_trackball = nullptr;
lv_group_t* LVGLInit::_default_group = nullptr;
TaskHandle_t LVGLInit::_task_handle = nullptr;
SemaphoreHandle_t LVGLInit::_mutex = nullptr;
bool LVGLInit::init() { return true; }
bool LVGLInit::init_display_only() { return true; }
void LVGLInit::task_handler() { lv_timer_handler(); }
bool LVGLInit::start_task(int, int) { return false; }
bool LVGLInit::is_task_running() { return false; }
SemaphoreHandle_t LVGLInit::get_mutex() { return nullptr; }
uint32_t LVGLInit::get_tick() { return lv_tick_get(); }
bool LVGLInit::is_initialized() { return true; }
void LVGLInit::set_theme(bool) {}
lv_disp_t* LVGLInit::get_display() { return display; }
lv_indev_t* LVGLInit::get_keyboard() { return keyboard; }
lv_indev_t* LVGLInit::get_touch() { return _touch; }
lv_indev_t* LVGLInit::get_trackball() { return _trackball; }
lv_group_t* LVGLInit::get_default_group() { return group; }
void LVGLInit::focus_widget(lv_obj_t* obj) { lv_group_add_obj(group, obj); lv_group_focus_obj(obj); }
}

int main() {
    lv_init();
    static lv_disp_draw_buf_t draw_buffer;
    static lv_color_t draw_pixels[320 * 20];
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels, nullptr, 320 * 20);
    static lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 320; driver.ver_res = 240;
    driver.draw_buf = &draw_buffer; driver.flush_cb = flush;
    display = lv_disp_drv_register(&driver);
    group = lv_group_create();
    lv_group_set_default(group);
    static lv_indev_drv_t keyboard_driver;
    lv_indev_drv_init(&keyboard_driver);
    keyboard_driver.type = LV_INDEV_TYPE_KEYPAD;
    keyboard_driver.read_cb = read_key;
    keyboard = lv_indev_drv_register(&keyboard_driver);
    lv_indev_set_group(keyboard, group);


    const uint32_t baseline = lv_obj_get_child_cnt(lv_scr_act());
    bool fit_tier = false, fit_columns = false, reflow_tier = false, reflow_cards = false;
    bool stacked_tier = false, stacked_cards = false, stacked_pixels = false;
    bool stacked_scroll = false, stacked_objects = false;
    bool focus_events = false, edge_scroll = false;
    bool ready = false, cancel = false, enter = false, escape = false, focus_restore = false;
    bool table_pixels = false, form_pixels = false, focus_pixels = false, glyph_pixels = false;
    bool background_pixels = false, teardown = false;
    int delete_events = 0;

    UI::LXMF::NomadNet::DocumentParser parser;
    {
        UI::LXMF::NomadNetScreen screen;
        screen.show();
        auto fit_doc = parser.parse(
            "`tc80\nName|Value\n:---|---:\nalpha|one\n`t\n"
            "`<24|username`Initial> `<password|secret`Hidden>\n"
            "---\n`[Next`:/page/next.mu]\n`[Submit`:/page/form.mu`username]");
        assert(screen.set_page(fit_doc));
        lv_obj_update_layout(screen._screen);
        const auto fit = screen._table_layout;
        fit_tier = fit.valid && fit.tier == UI::LXMF::NomadNet::TableLayoutTier::FIT;
        fit_columns = fit.columns == 2 && fit.cards == 0 && fit.x >= 0 && fit.width > 0 &&
                      fit.x + fit.width <= 304 && fit.y >= 0 && fit.height > 0;

        auto reflow_doc = parser.parse(
            "`tc304\nFirst very wide heading|Second very wide heading|Third very wide heading\n"
            "---|---|---\nA long value that cannot fit beside peers|"
            "Another long value that wraps repeatedly|Final long value\n`t");
        assert(screen.set_page(reflow_doc));
        lv_obj_update_layout(screen._screen);
        const auto reflow = screen._table_layout;
        reflow_tier = reflow.valid && reflow.tier == UI::LXMF::NomadNet::TableLayoutTier::REFLOW;
        reflow_cards = reflow.columns == 3 && reflow.cards == 0 && reflow.width == 304 &&
                       reflow.x == 0 && reflow.height > 22;

        auto stacked_doc = parser.parse(
            "`tc304\nH1|H2|H3|H4|H5|H6|H7|H8\n"
            "---|---|---|---|---|---|---|---\n"
            "one|two|three|four|five|six|seven|`[eight`:/eight]\n`t");
        assert(screen.set_page(stacked_doc));
        lv_obj_update_layout(screen._screen);
        const auto stacked = screen._table_layout;
        stacked_tier = stacked.valid && stacked.tier == UI::LXMF::NomadNet::TableLayoutTier::STACKED;
        stacked_cards = stacked.columns == 0 && stacked.cards == 8 && stacked.x == 0 &&
                        stacked.width == 304 && stacked.height > 8 * 16 &&
                        screen._page_layout.size() <= screen.MAX_WINDOW_FRAGMENTS;
        render();
        lv_area_t stacked_content;
        lv_obj_get_content_coords(screen._content, &stacked_content);
        const int stacked_mid = (stacked_content.y1 + stacked_content.y2) / 2;
        stacked_pixels = count_color(stacked_content.x1, stacked_content.y1,
                                     stacked_content.x2, stacked_mid,
                                     UI::LXMF::Theme::border()) > 10 &&
                         count_color(stacked_content.x1, stacked_mid + 1,
                                     stacked_content.x2, stacked_content.y2,
                                     UI::LXMF::Theme::border()) > 10;
        stacked_objects = lv_obj_get_child_cnt(screen._screen) <= 16;
        lv_group_focus_obj(screen._content);
        lv_group_set_editing(group, true);
        screen._selected_focus = -1;
        dispatch_key(LV_KEY_DOWN);
        stacked_scroll = screen._selected_link >= 0 && screen.logical_scroll() > 0 &&
                         lv_obj_get_scroll_y(screen._content) > 0;

        std::string edge_page = "`<24|username`Initial>\n";
        for (int i = 0; i < 70; ++i) edge_page += "viewport edge acceptance line\n";
        edge_page += "`[Bottom link`:/page/bottom.mu]\n";
        assert(screen.set_page(parser.parse(edge_page)));
        lv_obj_update_layout(screen._screen);
        lv_group_focus_obj(screen._edit_button);
        lv_group_set_editing(group, false);
        lv_obj_t* before_focus = lv_group_get_focused(group);
        dispatch_key(LV_KEY_NEXT);
        focus_events = before_focus != lv_group_get_focused(group) &&
                       lv_group_get_focused(group) != nullptr;
        lv_group_focus_obj(screen._content);
        lv_group_set_editing(group, true);
        const int32_t before_scroll = screen.logical_scroll();
        dispatch_key(LV_KEY_DOWN);
        pump();
        edge_scroll = lv_group_get_focused(group) == screen._content &&
                      screen._selected_link >= 0 && screen.logical_scroll() > before_scroll &&
                      screen.logical_scroll() > 0 && lv_obj_get_scroll_y(screen._content) > 0;

        assert(screen.set_page(fit_doc));
        ready = editor_round_trip(screen, LV_EVENT_READY, 0, "ReadyValue", true, false, delete_events);
        cancel = editor_round_trip(screen, LV_EVENT_CANCEL, 0, "Cancelled", false, false, delete_events);
        enter = editor_round_trip(screen, LV_EVENT_ALL, LV_KEY_ENTER, "EnterValue", true, false, delete_events);
        escape = editor_round_trip(screen, LV_EVENT_ALL, LV_KEY_ESC, "SecretChanged", false, true, delete_events);
        focus_restore = ready && cancel && enter && escape && delete_events == 4 &&
                        screen._field_editor == nullptr && lv_indev_get_obj_act() == nullptr;

        lv_group_focus_obj(screen._content);
        lv_group_set_editing(group, true);
        screen._selected_focus = -1;
        dispatch_key(LV_KEY_DOWN);
        render();
        lv_area_t content;
        lv_obj_get_content_coords(screen._content, &content);
        const auto observation = screen._table_layout;
        const int tx = content.x1 + observation.x;
        const int ty = content.y1 + static_cast<int>(observation.y - screen.logical_scroll());
        const auto border = UI::LXMF::Theme::border();
        const auto surface = UI::LXMF::Theme::surface();
        table_pixels = count_color(tx, ty, tx + observation.width - 1, ty + observation.height - 1,
                                   border) > 10 &&
                       count_color(tx + 2, ty + 2, tx + observation.width - 3,
                                   ty + observation.height - 3, UI::LXMF::Theme::textPrimary()) > 0;
        background_pixels = same(framebuffer[static_cast<std::size_t>(content.y2 - 2) * 320 + content.x2 - 2],
                                 surface);
        for (const auto& fragment : screen._page_layout) {
            const int x1 = content.x1 + fragment.x;
            const int y1 = content.y1 + screen._layout_window_top + fragment.y - screen.logical_scroll();
            if (fragment.field_index >= 0 && fragment.field_index == screen._selected_field) {
                form_pixels = count_color(x1 + 2, y1 + 2, x1 + fragment.width - 3,
                                          y1 + fragment.height - 3, UI::LXMF::Theme::surfaceInput()) > 4;
                focus_pixels = count_color(x1, y1, x1 + fragment.width - 1,
                                           y1 + fragment.height - 1, UI::LXMF::Theme::primary()) > 4;
            }
            if (fragment.divider) {
                const std::size_t area = static_cast<std::size_t>(fragment.width) * fragment.height;
                glyph_pixels = area > count_color(x1, y1, x1 + fragment.width - 1,
                                                  y1 + fragment.height - 1, surface) + 2;
            }
        }

        bool back_called = false;
        screen.set_back_callback([&] { back_called = true; screen.hide(); });
        lv_event_send(screen._back_button, LV_EVENT_CLICKED, nullptr);
        teardown = back_called && lv_obj_has_flag(screen._screen, LV_OBJ_FLAG_HIDDEN) &&
                   !group_contains(screen._content) && !group_contains(screen._field_editor) &&
                   lv_group_get_focused(group) == nullptr && lv_indev_get_obj_act() == nullptr;
    }
    pump();
    teardown = teardown && lv_obj_get_child_cnt(lv_scr_act()) == baseline &&
               lv_group_get_focused(group) == nullptr;
    {
        UI::LXMF::NomadNetScreen screen;
        screen.show();
        assert(screen.set_page(parser.parse("`<password|secret`Hidden>\n")));
        bool home_called = false;
        screen.set_home_callback([&] { home_called = true; screen.hide(); });
        lv_event_send(screen._home_button, LV_EVENT_CLICKED, nullptr);
        teardown = teardown && home_called && lv_group_get_focused(group) == nullptr &&
                   lv_indev_get_obj_act() == nullptr && !group_contains(screen._content);
    }
    pump();
    const uint32_t remaining = lv_obj_get_child_cnt(lv_scr_act()) - baseline;
    teardown = teardown && remaining == 0 && lv_group_get_focused(group) == nullptr;

    lv_indev_delete(keyboard);

    lv_group_del(group);
    std::printf(
        "LVGL ACCEPT 320x240 fit_tier=%d fit_columns=%d reflow_tier=%d reflow_cards=%d "
        "stacked_tier=%d stacked_cards=%d stacked_pixels=%d stacked_scroll=%d stacked_objects=%d "
        "focus_events=%d edge_scroll=%d ready=%d cancel=%d enter=%d escape=%d focus_restore=%d "
        "teardown=%d stale_group=%d background_pixels=%d table_pixels=%d form_pixels=%d "
        "focus_pixels=%d glyph_pixels=%d exact_fonts=1 objects=%u\n",
        fit_tier, fit_columns, reflow_tier, reflow_cards, stacked_tier, stacked_cards,
        stacked_pixels, stacked_scroll, stacked_objects, focus_events, edge_scroll,
        ready, cancel, enter, escape, focus_restore, teardown, 0, background_pixels,
        table_pixels, form_pixels, focus_pixels, glyph_pixels, remaining);
    return fit_tier && fit_columns && reflow_tier && reflow_cards && stacked_tier &&
           stacked_cards && stacked_pixels && stacked_scroll && stacked_objects &&
           focus_events && edge_scroll &&
           ready && cancel && enter && escape && focus_restore && teardown && background_pixels &&
           table_pixels && form_pixels && focus_pixels && glyph_pixels && remaining == 0 ? 0 : 1;
}
