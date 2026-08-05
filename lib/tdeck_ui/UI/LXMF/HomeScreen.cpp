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
    static lv_coord_t rows[] = {30, LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(_screen, cols, rows);
    lv_obj_t* title = lv_label_create(_screen);
    lv_label_set_text(title, "Pyxis Home");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_grid_cell(title, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 0, 1);
    static const char* labels[] = {"Messages", "NomadNet", "Network", "Settings"};
    for (int i = 0; i < 4; ++i) {
        _buttons[i] = lv_btn_create(_screen);
        lv_obj_set_grid_cell(_buttons[i], LV_GRID_ALIGN_STRETCH, i % 2, 1,
                             LV_GRID_ALIGN_STRETCH, 1 + i / 2, 1);
        lv_obj_set_style_bg_color(_buttons[i], Theme::surfaceContainer(), 0);
        lv_obj_set_style_bg_color(_buttons[i], Theme::surfaceElevated(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(_buttons[i], Theme::info(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(_buttons[i], 2, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(_buttons[i], clicked, LV_EVENT_CLICKED, this);
        lv_obj_t* label = lv_label_create(_buttons[i]);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_color(label, Theme::textPrimary(), 0);
        lv_obj_center(label);
    }
    hide();
}
HomeScreen::~HomeScreen() { if (_screen) lv_obj_del(_screen); }
void HomeScreen::show() {
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(_screen);
    auto* group = LVGL::LVGLInit::get_default_group();
    if (group) { for (auto* button : _buttons) lv_group_add_obj(group, button); lv_group_focus_obj(_buttons[0]); }
}
void HomeScreen::hide() {
    auto* group = LVGL::LVGLInit::get_default_group();
    if (group) for (auto* button : _buttons) lv_group_remove_obj(button);
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}
void HomeScreen::clicked(lv_event_t* event) {
    auto* self = static_cast<HomeScreen*>(lv_event_get_user_data(event));
    auto* target = lv_event_get_target(event);
    if (target == self->_buttons[0] && self->_messages) self->_messages();
    else if (target == self->_buttons[1] && self->_nomadnet) self->_nomadnet();
    else if (target == self->_buttons[2] && self->_network) self->_network();
    else if (target == self->_buttons[3] && self->_settings) self->_settings();
}
}
#endif
