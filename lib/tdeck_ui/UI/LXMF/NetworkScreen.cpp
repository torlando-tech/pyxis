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
    lv_obj_t* title = lv_label_create(header); lv_label_set_text(title, "Network"); lv_obj_center(title);
    lv_obj_add_event_cb(_back_button, clicked, LV_EVENT_CLICKED, this); lv_obj_add_event_cb(_home_button, clicked, LV_EVENT_CLICKED, this);
    lv_obj_t* content = lv_obj_create(_screen); lv_obj_set_size(content, LV_PCT(100), 190); lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_gap(content, 6, 0);
    static const char* labels[] = {"Announces", "Status", "Radio Activity", "Propagation Nodes"};
    for (int i = 0; i < 4; ++i) {
        _buttons[i] = lv_btn_create(content); lv_obj_set_size(_buttons[i], LV_PCT(100), 38);
        lv_obj_set_style_bg_color(_buttons[i], Theme::surfaceContainer(), 0);
        lv_obj_set_style_bg_color(_buttons[i], Theme::surfaceElevated(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(_buttons[i], Theme::info(), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(_buttons[i], 2, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(_buttons[i], clicked, LV_EVENT_CLICKED, this);
        lv_obj_t* label = lv_label_create(_buttons[i]); lv_label_set_text(label, labels[i]); lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    }
    hide();
}
NetworkScreen::~NetworkScreen() { if (_screen) lv_obj_del(_screen); }
void NetworkScreen::show() { lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(_screen); auto* g=LVGL::LVGLInit::get_default_group(); if(g){lv_group_add_obj(g,_back_button);lv_group_add_obj(g,_home_button);for(auto* b:_buttons)lv_group_add_obj(g,b);lv_group_focus_obj(_buttons[0]);}}
void NetworkScreen::hide() { auto* g=LVGL::LVGLInit::get_default_group(); if(g){lv_group_remove_obj(_back_button);lv_group_remove_obj(_home_button);for(auto* b:_buttons)lv_group_remove_obj(b);} lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN); }
void NetworkScreen::clicked(lv_event_t* e) { auto* s=static_cast<NetworkScreen*>(lv_event_get_user_data(e)); auto* t=lv_event_get_target(e); if(t==s->_back_button&&s->_back)s->_back(); else if(t==s->_home_button&&s->_home)s->_home(); else for(int i=0;i<4;++i)if(t==s->_buttons[i]&&s->_callbacks[i])s->_callbacks[i](); }
}
#endif
