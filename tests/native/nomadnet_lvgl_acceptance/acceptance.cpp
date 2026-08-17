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
bool fail_next_timer_create = false;

extern "C" lv_timer_t* __real_lv_timer_create(lv_timer_cb_t, uint32_t, void*);
extern "C" lv_timer_t* __wrap_lv_timer_create(lv_timer_cb_t callback, uint32_t period,
                                                void* user_data) {
    if (fail_next_timer_create) {
        fail_next_timer_create = false;
        return nullptr;
    }
    return __real_lv_timer_create(callback, period, user_data);
}

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
    bool eight_column_tier = false, eight_column_preserved = false, eight_column_pixels = false;
    bool table_link_focus = false, eight_column_objects = false;
    bool focus_events = false, edge_scroll = false;
    bool ready = false, cancel = false, enter = false, escape = false, focus_restore = false;
    bool table_pixels = false, form_pixels = false, focus_pixels = false, glyph_pixels = false;
    bool background_pixels = false, teardown = false, cached_status_transient = false;
    bool cached_status_oom_collapses = false;
    bool partial_replace = false, partial_forms = false, partial_empty = false;
    bool partial_link_focus = false, partial_focus_fallback = false;
    bool partial_scroll_anchor = false;
    bool partial_second_scroll_rollback = false;
    bool partial_region_top_fallback = false;
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
        screen.set_status("Cached page; current reachability not checked");
        lv_obj_update_layout(screen._screen);
        const int16_t content_with_cached_notice = lv_obj_get_height(screen._content);
        const bool cached_notice_visible =
            !lv_obj_has_flag(screen._status, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 12; ++i) pump();
        lv_obj_update_layout(screen._screen);
        cached_status_transient = cached_notice_visible &&
            lv_obj_has_flag(screen._status, LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_get_height(screen._content) > content_with_cached_notice;
        screen.set_status("Checking page cache...");
        fail_next_timer_create = true;
        screen.set_status("Cached page; current reachability not checked");
        lv_obj_update_layout(screen._screen);
        cached_status_oom_collapses = !fail_next_timer_create &&
            screen._status_timer == nullptr &&
            lv_obj_has_flag(screen._status, LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_get_height(screen._content) > content_with_cached_notice;
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

        auto eight_column_doc = parser.parse(
            "`tl304\nA|B|C|D|E|F|G|H\n"
            "---|---|---|---|---|---|---|---\n"
            "Alpha|Bravo|Charlie|Delta|Echo|Foxtrot|Golf|`[Hotel`:/hotel]\n"
            "One|Two|Three|Four|Five|Six|Seven|Eight\n`t");
        assert(screen.set_page(eight_column_doc));
        lv_obj_update_layout(screen._screen);
        const auto eight_column = screen._table_layout;
        eight_column_tier = eight_column.valid &&
            eight_column.tier == UI::LXMF::NomadNet::TableLayoutTier::REFLOW;
        eight_column_preserved = eight_column.columns == 8 && eight_column.cards == 0 &&
            eight_column.x == 0 && eight_column.width == 304 && eight_column.height > 22 &&
            screen._page_layout.size() <= screen.MAX_WINDOW_FRAGMENTS;
        render();
        lv_area_t eight_column_content;
        lv_obj_get_content_coords(screen._content, &eight_column_content);
        const int eight_column_mid =
            (eight_column_content.y1 + eight_column_content.y2) / 2;
        eight_column_pixels = count_color(
            eight_column_content.x1, eight_column_content.y1,
            eight_column_content.x2, eight_column_mid,
            UI::LXMF::Theme::border()) > 10 &&
            count_color(eight_column_content.x1, eight_column_mid + 1,
                        eight_column_content.x2, eight_column_content.y2,
                        UI::LXMF::Theme::border()) > 10;
        eight_column_objects = lv_obj_get_child_cnt(screen._screen) <= 16;
        lv_group_focus_obj(screen._content);
        lv_group_set_editing(group, true);
        screen._selected_focus = -1;
        dispatch_key(LV_KEY_DOWN);
        table_link_focus = screen._selected_link >= 0;

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
    {
        UI::LXMF::NomadNetScreen screen;
        screen.show();
        const auto page = parser.parse(
            "`<24|username`Initial>\n"
            "`{:/partial.mu`10`pid=clock|username}\nfooter");
        assert(screen.set_page(page));
        assert(screen._form_state.set_value(0, "User edit"));
        UI::LXMF::NomadNet::PartialScheduler scheduler;
        scheduler.configure(page, 7, 0);
        UI::LXMF::NomadNet::PartialRequest request;
        assert(scheduler.poll(0, true, true, request));
        UI::LXMF::NomadNet::PartialController controller;
        controller.reset_page(page.source_bytes);
        UI::LXMF::NomadNet::FormEncodeResult encode_result;
        assert(screen.prepare_partial_request(request, controller, encode_result));
        const auto fragment = parser.parse(
            "Updated `<24|username`Server>\n"
            "`[Open`:/next.mu]\n"
            "`tc80\nA|B\n---|---\nOne|`[Two`:/two]\n`t\n"
            "`{:nested.mu}");
        assert(screen.apply_partial_fragment(request, fragment, controller) ==
               UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        partial_replace = screen._page.partials().size() == 1 &&
            screen._page.blocks().size() >= 4 &&
            screen._page.blocks()[1].partial_region_index == 0 &&
            screen._page.tables().size() == 1 && screen._page.links().size() == 2 &&
            screen.partial_id_matches(0, "clock", 5);
        partial_forms = screen._form_state.fields().size() == 2 &&
            std::string(screen._form_state.fields()[0].value.data(),
                        screen._form_state.fields()[0].value_length) == "User edit";
        screen._selected_link = 0;
        for (std::size_t index = 0; index < screen._focus_order.size(); ++index) {
            if (!screen._focus_order[index].field &&
                    screen._focus_order[index].index == 0) {
                screen._selected_focus = static_cast<int16_t>(index);
                break;
            }
        }
        assert(screen.apply_partial_fragment(request, fragment, controller) ==
               UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        partial_link_focus = screen._selected_link == 0 && screen._selected_focus >= 0;
        const auto empty_fragment = parser.parse("");
        assert(screen.apply_partial_fragment(request, empty_fragment, controller) ==
               UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        partial_empty = screen._page.partials().size() == 1 &&
            std::any_of(screen._page.blocks().begin(), screen._page.blocks().end(),
                [](const auto& block) { return block.partial_region_index == 0; });

        const auto focus_page = parser.parse("`{:/focus.mu`10}\n");
        assert(screen.set_page(focus_page));
        scheduler.configure(focus_page, 8, 0);
        assert(scheduler.poll(0, true, true, request));
        controller.cancel();
        controller.reset_page(focus_page.source_bytes);
        assert(screen.prepare_partial_request(request, controller, encode_result));
        assert(screen.apply_partial_fragment(request, parser.parse(
            "`[First`:/first]\n`[Gone`:/gone]\n`[Third`:/third]"),
            controller) == UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        for (std::size_t index = 0; index < screen._page.links().size(); ++index) {
            const auto target = screen._page.target(index);
            if (std::string(target.data(), target.size()) != ":/gone") continue;
            screen._selected_link = static_cast<int16_t>(index);
            for (std::size_t focus = 0; focus < screen._focus_order.size(); ++focus)
                if (!screen._focus_order[focus].field &&
                        screen._focus_order[focus].index == index)
                    screen._selected_focus = static_cast<int16_t>(focus);
        }
        assert(screen.apply_partial_fragment(request, parser.parse(
            "`[First`:/first]\n`[Third`:/third]"), controller) ==
            UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        partial_focus_fallback = screen._selected_link >= 0 &&
            [&] {
                const auto target = screen._page.target(
                    static_cast<std::size_t>(screen._selected_link));
                return std::string(target.data(), target.size()) == ":/third";
            }();

        std::string scroll_source = "`{:/scroll.mu`10}\n";
        for (int index = 0; index < 30; ++index)
            scroll_source += "Base " + std::to_string(index) + "\n\n";
        const auto scroll_page = parser.parse(scroll_source);
        assert(screen.set_page(scroll_page));
        scheduler.configure(scroll_page, 9, 0);
        assert(scheduler.poll(0, true, true, request));
        controller.cancel();
        controller.reset_page(scroll_page.source_bytes);
        assert(screen.prepare_partial_request(request, controller, encode_result));
        assert(screen.apply_partial_fragment(request, parser.parse("short"), controller) ==
            UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        assert(screen.scroll_to_logical(120, LV_ANIM_OFF));
        auto top_identity = [&]() {
            int16_t region = -2;
            std::size_t ordinal = 0;
            const UI::LXMF::NomadNetScreen::LayoutCheckpoint* top = nullptr;
            for (const auto& checkpoint : screen._layout_checkpoints)
                if (checkpoint.y <= screen._logical_scroll &&
                        (!top || checkpoint.y >= top->y)) top = &checkpoint;
            if (!top || top->block_index >= screen._page.blocks().size())
                return std::pair<int16_t, std::size_t>{region, ordinal};
            region = screen._page.blocks()[top->block_index].partial_region_index;
            for (std::size_t index = 0; index < top->block_index; ++index)
                if (screen._page.blocks()[index].partial_region_index == region) ++ordinal;
            return std::pair<int16_t, std::size_t>{region, ordinal};
        };
        const auto before_top = top_identity();
        std::string expanded;
        for (int index = 0; index < 18; ++index)
            expanded += "Expanded " + std::to_string(index) + "\n\n";
        assert(screen.apply_partial_fragment(request, parser.parse(expanded), controller) ==
            UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        partial_scroll_anchor = before_top == top_identity();
        const auto rollback_top = top_identity();
        const int32_t rollback_logical = screen._logical_scroll;
        const int32_t rollback_widget = lv_obj_get_scroll_y(screen._content);
        const std::size_t rollback_blocks = screen._page.blocks().size();
        screen._test_scroll_fail_countdown = 1;
        const auto rollback_result = screen.apply_partial_fragment(
            request, parser.parse("replacement\n\nwith another block"), controller);
        partial_second_scroll_rollback =
            rollback_result == UI::LXMF::NomadNet::PartialReplaceResult::ALLOCATION_FAILED &&
            screen._logical_scroll == rollback_logical &&
            lv_obj_get_scroll_y(screen._content) == rollback_widget &&
            screen._page.blocks().size() == rollback_blocks &&
            top_identity() == rollback_top;

        std::string tall_fragment;
        for (int line = 0; line < 18; ++line)
            tall_fragment += "partial " + std::to_string(line) + "\n\n";
        assert(screen.apply_partial_fragment(
            request, parser.parse(tall_fragment), controller) ==
            UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        std::size_t partial_seen = 0;
        int32_t removed_anchor_y = -1;
        for (const auto& checkpoint : screen._layout_checkpoints) {
            if (screen._page.blocks()[checkpoint.block_index].partial_region_index != 0)
                continue;
            if (partial_seen++ == 8) {
                removed_anchor_y = checkpoint.y;
                break;
            }
        }
        assert(removed_anchor_y >= 0);
        assert(screen.scroll_to_logical(removed_anchor_y + 1, LV_ANIM_OFF));
        assert(screen.apply_partial_fragment(
            request, parser.parse("short replacement"), controller) ==
            UI::LXMF::NomadNet::PartialReplaceResult::APPLIED);
        const auto region_fallback = top_identity();
        int32_t expected_region_top = 0;
        for (const auto& checkpoint : screen._layout_checkpoints) {
            if (screen._page.blocks()[checkpoint.block_index].partial_region_index == 0) {
                expected_region_top = checkpoint.y;
                break;
            }
        }
        const int32_t viewport = std::max<int32_t>(
            1, lv_obj_get_content_height(screen._content));
        expected_region_top = std::min(
            expected_region_top, std::max<int32_t>(0, screen._page_height - viewport));
        partial_region_top_fallback =
            std::get<0>(region_fallback) == 0 &&
            std::get<1>(region_fallback) == 0 &&
            screen._logical_scroll == expected_region_top;
        screen.hide();
    }
    pump();
    const uint32_t remaining = lv_obj_get_child_cnt(lv_scr_act()) - baseline;
    teardown = teardown && remaining == 0 && lv_group_get_focused(group) == nullptr;

    lv_indev_delete(keyboard);

    lv_group_del(group);
    std::printf(
        "LVGL ACCEPT 320x240 fit_tier=%d fit_columns=%d reflow_tier=%d reflow_cards=%d "
        "eight_column_tier=%d eight_column_preserved=%d eight_column_pixels=%d table_link_focus=%d eight_column_objects=%d "
        "focus_events=%d edge_scroll=%d ready=%d cancel=%d enter=%d escape=%d focus_restore=%d "
        "teardown=%d cached_status_transient=%d cached_status_oom_collapses=%d stale_group=%d background_pixels=%d table_pixels=%d form_pixels=%d "
        "focus_pixels=%d glyph_pixels=%d partial_replace=%d partial_forms=%d partial_link_focus=%d partial_focus_fallback=%d partial_scroll_anchor=%d partial_second_scroll_rollback=%d partial_region_top_fallback=%d partial_empty=%d exact_fonts=1 objects=%u\n",
        fit_tier, fit_columns, reflow_tier, reflow_cards, eight_column_tier, eight_column_preserved,
        eight_column_pixels, table_link_focus, eight_column_objects, focus_events, edge_scroll,
        ready, cancel, enter, escape, focus_restore, teardown, cached_status_transient,
        cached_status_oom_collapses, 0, background_pixels,
        table_pixels, form_pixels, focus_pixels, glyph_pixels,
        partial_replace, partial_forms, partial_link_focus, partial_focus_fallback,
        partial_scroll_anchor, partial_second_scroll_rollback,
        partial_region_top_fallback, partial_empty, remaining);
    return fit_tier && fit_columns && reflow_tier && reflow_cards && eight_column_tier &&
           eight_column_preserved && eight_column_pixels && table_link_focus && eight_column_objects &&
           focus_events && edge_scroll &&
           ready && cancel && enter && escape && focus_restore && teardown && cached_status_transient &&
           cached_status_oom_collapses && background_pixels &&
           table_pixels && form_pixels && focus_pixels && glyph_pixels &&
           partial_replace && partial_forms && partial_link_focus && partial_focus_fallback &&
           partial_scroll_anchor && partial_second_scroll_rollback &&
           partial_region_top_fallback && partial_empty &&
           remaining == 0 ? 0 : 1;
}
