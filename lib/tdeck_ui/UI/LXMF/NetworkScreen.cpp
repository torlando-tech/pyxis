#include "NetworkScreen.h"
#ifdef ARDUINO
#include "Theme.h"
#include "../LVGL/LVGLInit.h"
namespace UI::LXMF {
NetworkScreen::NetworkScreen() {
    _screen = lv_obj_create(lv_scr_act()); lv_obj_set_size(_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(_screen, Theme::surface(), 0); lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_pad_all(_screen, 8, 0);
    lv_obj_t* header = lv_obj_create(_screen); lv_obj_set_size(header, LV_PCT(100), 34); lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, Theme::surfaceHeader(), 0); lv_obj_set_style_border_width(header, 0, 0); lv_obj_set_style_pad_all(header, 2, 0);
    _back_button = lv_btn_create(header); lv_obj_set_size(_back_button, 44, 28); lv_obj_align(_back_button, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* back = lv_label_create(_back_button); lv_label_set_text(back, LV_SYMBOL_LEFT); lv_obj_center(back);
    _home_button = lv_btn_create(header); lv_obj_set_size(_home_button, 44, 28); lv_obj_align(_home_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t* home = lv_label_create(_home_button); lv_label_set_text(home, LV_SYMBOL_HOME); lv_obj_center(home);
    lv_obj_t* title = lv_label_create(header); lv_label_set_text(title, "Network"); lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0); lv_obj_center(title);
    for(auto* button:{_back_button,_home_button}) {
        lv_obj_set_style_bg_color(button, Theme::surfaceContainer(), 0);
        lv_obj_set_style_bg_color(button, Theme::primaryPressed(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_radius(button, 8, 0);
    }
    lv_obj_add_event_cb(_back_button, clicked, LV_EVENT_CLICKED, this); lv_obj_add_event_cb(_home_button, clicked, LV_EVENT_CLICKED, this);
    lv_obj_t* content = lv_obj_create(_screen); lv_obj_set_size(content, LV_PCT(100), 190); lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0); lv_obj_set_style_pad_gap(content, 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    static const char* labels[] = {"Status", "Radio Activity", "Propagation Nodes"};
    static const char* details[] = {"Interfaces & storage", "Signal history", "Delivery relays"};
    static const char* symbols[] = {LV_SYMBOL_LIST, LV_SYMBOL_WIFI, LV_SYMBOL_UPLOAD};
    for (int i = 0; i < 3; ++i) {
        _buttons[i] = lv_btn_create(content); lv_obj_set_size(_buttons[i], LV_PCT(100), 58);
        lv_obj_set_style_bg_color(_buttons[i], Theme::surfaceContainer(), 0);
        lv_obj_set_style_bg_color(_buttons[i], Theme::primaryPressed(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(_buttons[i], 0, 0);
        lv_obj_set_style_outline_color(_buttons[i], Theme::primaryLight(), LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(_buttons[i], 1, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(_buttons[i], 1, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(_buttons[i], 8, 0);
        lv_obj_set_style_pad_all(_buttons[i], 4, 0);
        lv_obj_add_event_cb(_buttons[i], clicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon = lv_label_create(_buttons[i]); lv_label_set_text(icon, symbols[i]);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(icon, Theme::primaryLight(), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t* label = lv_label_create(_buttons[i]); lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, Theme::textPrimary(), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 32, -7);
        lv_obj_t* detail = lv_label_create(_buttons[i]); lv_label_set_text(detail, details[i]);
        lv_obj_set_style_text_font(detail, &lv_font_montserrat_8, 0);
        lv_obj_set_style_text_color(detail, Theme::textTertiary(), 0);
        lv_obj_align(detail, LV_ALIGN_LEFT_MID, 32, 10);
    }
    hide();
}
NetworkScreen::~NetworkScreen() { if (_screen) lv_obj_del(_screen); }
void NetworkScreen::show() {
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);
    auto* g = LVGL::LVGLInit::get_default_group();
    if (g) {
        lv_obj_t* objects[] = {_back_button, _home_button, _buttons[0], _buttons[1], _buttons[2]};
        lv_group_focus_freeze(g, true);
        for (auto* object : objects) lv_group_add_obj(g, object);
        lv_group_focus_freeze(g, false);
        lv_group_focus_obj(_buttons[0]);
    }
}
void NetworkScreen::hide() {
    auto* g = LVGL::LVGLInit::get_default_group();
    if (g) {
        lv_obj_t* objects[] = {_back_button, _home_button, _buttons[0], _buttons[1], _buttons[2]};
        lv_obj_t* focused = lv_group_get_focused(g);
        bool focused_is_network = false;
        for (auto* object : objects) {
            if (object == focused) {
                focused_is_network = true;
            } else {
                lv_group_remove_obj(object);
            }
        }
        if (focused_is_network) lv_group_remove_obj(focused);
    }
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}
void NetworkScreen::clicked(lv_event_t* e) { auto* s=static_cast<NetworkScreen*>(lv_event_get_user_data(e)); auto* t=lv_event_get_target(e); if(t==s->_back_button&&s->_back)s->_back(); else if(t==s->_home_button&&s->_home)s->_home(); else for(int i=0;i<3;++i)if(t==s->_buttons[i]&&s->_callbacks[i])s->_callbacks[i](); }
}
#endif
