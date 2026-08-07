#include "HomeScreen.h"
#ifdef ARDUINO
#include "Theme.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"

namespace UI::LXMF {
HomeScreen::HomeScreen() {
    _screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_screen, Theme::surface(), 0);
    lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_pad_all(_screen, 10, 0);
    lv_obj_set_style_pad_row(_screen, 6, 0);
    lv_obj_set_style_pad_column(_screen, 6, 0);
    lv_obj_set_layout(_screen, LV_LAYOUT_GRID);
    static lv_coord_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[] = {
        30, LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(_screen, cols, rows);
    lv_obj_t* title = lv_label_create(_screen);
    lv_label_set_text(title, "Pyxis Home");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_grid_cell(title, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 0, 1);
    static const char* labels[] = {
        "Messages", "NomadNet", "Network", "Settings", "Maps"};
    static const char* details[] = {
        "Inbox & calls", "Browse Micron", "Links & radio", "Device options",
        "Offline SD packs"};
    static const char* symbols[] = {
        LV_SYMBOL_ENVELOPE, LV_SYMBOL_DIRECTORY, LV_SYMBOL_WIFI,
        LV_SYMBOL_SETTINGS, LV_SYMBOL_GPS};
    for (std::size_t i = 0; i < APP_COUNT; ++i) {
        _buttons[i] = lv_btn_create(_screen);
        const bool wide_last_tile = i == APP_COUNT - 1U;
        lv_obj_set_grid_cell(
            _buttons[i], LV_GRID_ALIGN_STRETCH,
            wide_last_tile ? 0 : static_cast<int>(i % 2U),
            wide_last_tile ? 2 : 1,
            LV_GRID_ALIGN_STRETCH, 1 + static_cast<int>(i / 2U), 1);
        lv_obj_set_style_bg_color(_buttons[i], Theme::surfaceContainer(), 0);
        lv_obj_set_style_bg_color(_buttons[i], Theme::primaryPressed(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(_buttons[i], 0, 0);
        lv_obj_set_style_outline_color(_buttons[i], Theme::primaryLight(), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(_buttons[i], 1, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(_buttons[i], 1, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(_buttons[i], 10, 0);
        lv_obj_set_style_pad_all(_buttons[i], 6, 0);
        lv_obj_add_event_cb(_buttons[i], clicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon = lv_label_create(_buttons[i]);
        lv_label_set_text(icon, symbols[i]);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(icon, Theme::primaryLight(), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* label = lv_label_create(_buttons[i]);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, Theme::textPrimary(), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 3);

        lv_obj_t* detail = lv_label_create(_buttons[i]);
        lv_label_set_text(detail, details[i]);
        lv_obj_set_style_text_font(detail, &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_color(detail, Theme::textTertiary(), 0);
        lv_obj_align(detail, LV_ALIGN_BOTTOM_LEFT, 24, -3);
    }
    hide();
}
HomeScreen::~HomeScreen() { if (_screen) lv_obj_del(_screen); }
void HomeScreen::show() {
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);
    auto* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Rebuild the ordered launcher group without letting the first add emit
        // an automatic focus event. Messages becomes the sole final owner.
        lv_group_focus_freeze(group, true);
        for (auto* button : _buttons) lv_group_add_obj(group, button);
        lv_group_focus_freeze(group, false);
        lv_group_focus_obj(_buttons[0]);
    }
}
void HomeScreen::hide() {
    auto* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Removing the current owner makes LVGL synchronously focus another
        // member. Detach every other launcher tile first and the owner last so
        // no later tile becomes a transient focus owner during navigation.
        lv_obj_t* focused = lv_group_get_focused(group);
        bool focused_is_launcher = false;
        for (auto* button : _buttons) {
            if (button == focused) {
                focused_is_launcher = true;
            } else {
                lv_group_remove_obj(button);
            }
        }
        if (focused_is_launcher) lv_group_remove_obj(focused);
    }
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}
void HomeScreen::clicked(lv_event_t* event) {
    auto* self = static_cast<HomeScreen*>(lv_event_get_user_data(event));
    auto* target = lv_event_get_target(event);
    if (target == self->_buttons[0] && self->_messages) self->_messages();
    else if (target == self->_buttons[1] && self->_nomadnet) self->_nomadnet();
    else if (target == self->_buttons[2] && self->_network) self->_network();
    else if (target == self->_buttons[3] && self->_settings) self->_settings();
    else if (target == self->_buttons[4] && self->_map) self->_map();
}
}
#endif
