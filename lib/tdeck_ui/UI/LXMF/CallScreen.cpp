// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "CallScreen.h"

#ifdef ARDUINO

#include "Theme.h"
#include "../LVGL/LVGLInit.h"

namespace UI {
namespace LXMF {

CallScreen::CallScreen(lv_obj_t* parent)
    : _screen(nullptr), _label_state(nullptr), _label_peer(nullptr),
      _label_duration(nullptr), _btn_mute(nullptr), _label_mute(nullptr),
      _btn_hangup(nullptr), _muted(false), _state(CallState::CONNECTING) {

    _screen = lv_obj_create(parent ? parent : lv_scr_act());
    lv_obj_set_size(_screen, 320, 240);
    lv_obj_set_style_bg_color(_screen, Theme::surface(), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(_screen, 0, 0);

    create_ui();
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

CallScreen::~CallScreen() {
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void CallScreen::create_ui() {
    // State label (top area) - "Connecting...", "Ringing...", "In Call", "Call Ended"
    _label_state = lv_label_create(_screen);
    lv_label_set_text(_label_state, "Connecting...");
    lv_obj_set_style_text_color(_label_state, Theme::textSecondary(), 0);
    lv_obj_set_style_text_font(_label_state, &lv_font_montserrat_16, 0);
    lv_obj_align(_label_state, LV_ALIGN_TOP_MID, 0, 30);

    // Peer name/hash label
    _label_peer = lv_label_create(_screen);
    lv_label_set_text(_label_peer, "");
    lv_obj_set_style_text_color(_label_peer, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(_label_peer, &lv_font_montserrat_14, 0);
    lv_obj_set_width(_label_peer, 280);
    lv_obj_set_style_text_align(_label_peer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_label_peer, LV_LABEL_LONG_DOT);
    lv_obj_align(_label_peer, LV_ALIGN_TOP_MID, 0, 60);

    // Duration timer label
    _label_duration = lv_label_create(_screen);
    lv_label_set_text(_label_duration, "");
    lv_obj_set_style_text_color(_label_duration, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(_label_duration, &lv_font_montserrat_16, 0);
    lv_obj_align(_label_duration, LV_ALIGN_CENTER, 0, -10);

    // Button row at bottom
    // Mute button (left)
    _btn_mute = lv_btn_create(_screen);
    lv_obj_set_size(_btn_mute, 100, 44);
    lv_obj_align(_btn_mute, LV_ALIGN_BOTTOM_LEFT, 30, -30);
    lv_obj_set_style_bg_color(_btn_mute, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(_btn_mute, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_btn_mute, Theme::surfaceElevated(), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(_btn_mute, 8, 0);
    lv_obj_add_event_cb(_btn_mute, on_mute_clicked, LV_EVENT_CLICKED, this);

    _label_mute = lv_label_create(_btn_mute);
    lv_label_set_text(_label_mute, LV_SYMBOL_AUDIO " Mute");
    lv_obj_set_style_text_color(_label_mute, Theme::textPrimary(), 0);
    lv_obj_center(_label_mute);

    // Hangup button (right) - red
    _btn_hangup = lv_btn_create(_screen);
    lv_obj_set_size(_btn_hangup, 100, 44);
    lv_obj_align(_btn_hangup, LV_ALIGN_BOTTOM_RIGHT, -30, -30);
    lv_obj_set_style_bg_color(_btn_hangup, lv_color_hex(0xC62828), 0);  // Dark red
    lv_obj_set_style_bg_color(_btn_hangup, lv_color_hex(0xB71C1C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(_btn_hangup, lv_color_hex(0xD32F2F), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(_btn_hangup, 8, 0);
    lv_obj_add_event_cb(_btn_hangup, on_hangup_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_hangup = lv_label_create(_btn_hangup);
    lv_label_set_text(label_hangup, LV_SYMBOL_CLOSE " End");
    lv_obj_set_style_text_color(label_hangup, lv_color_white(), 0);
    lv_obj_center(label_hangup);
}

void CallScreen::set_peer(const RNS::Bytes& peer_hash, const String& display_name) {
    if (display_name.length() > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n%.16s...", display_name.c_str(),
                 peer_hash.toHex().c_str());
        lv_label_set_text(_label_peer, buf);
    } else {
        lv_label_set_text(_label_peer, peer_hash.toHex().c_str());
    }
}

void CallScreen::set_state(CallState state) {
    _state = state;
    switch (state) {
        case CallState::CONNECTING:
            lv_label_set_text(_label_state, "Connecting...");
            lv_label_set_text(_label_duration, "");
            break;
        case CallState::RINGING:
            lv_label_set_text(_label_state, "Ringing...");
            break;
        case CallState::ACTIVE:
            lv_label_set_text(_label_state, LV_SYMBOL_CALL " In Call");
            lv_obj_set_style_text_color(_label_state, Theme::success(), 0);
            break;
        case CallState::ENDED:
            lv_label_set_text(_label_state, "Call Ended");
            lv_obj_set_style_text_color(_label_state, Theme::textSecondary(), 0);
            break;
    }
}

void CallScreen::set_duration(uint32_t seconds) {
    char buf[16];
    uint32_t m = seconds / 60;
    uint32_t s = seconds % 60;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    lv_label_set_text(_label_duration, buf);
}

void CallScreen::set_muted(bool muted) {
    _muted = muted;
    if (muted) {
        lv_label_set_text(_label_mute, LV_SYMBOL_MUTE " Muted");
        lv_obj_set_style_bg_color(_btn_mute, Theme::warning(), 0);
    } else {
        lv_label_set_text(_label_mute, LV_SYMBOL_AUDIO " Mute");
        lv_obj_set_style_bg_color(_btn_mute, Theme::btnSecondary(), 0);
    }
}

void CallScreen::set_hangup_callback(HangupCallback callback) {
    _hangup_callback = callback;
}

void CallScreen::set_mute_callback(MuteCallback callback) {
    _mute_callback = callback;
}

void CallScreen::show() {
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_mute) lv_group_add_obj(group, _btn_mute);
        if (_btn_hangup) lv_group_add_obj(group, _btn_hangup);
        lv_group_focus_obj(_btn_hangup);
    }
}

void CallScreen::hide() {
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);

    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_mute) lv_group_remove_obj(_btn_mute);
        if (_btn_hangup) lv_group_remove_obj(_btn_hangup);
    }
}

lv_obj_t* CallScreen::get_object() {
    return _screen;
}

void CallScreen::on_hangup_clicked(lv_event_t* event) {
    auto* self = static_cast<CallScreen*>(lv_event_get_user_data(event));
    if (self && self->_hangup_callback) {
        self->_hangup_callback();
    }
}

void CallScreen::on_mute_clicked(lv_event_t* event) {
    auto* self = static_cast<CallScreen*>(lv_event_get_user_data(event));
    if (self && self->_mute_callback) {
        self->_muted = !self->_muted;
        self->set_muted(self->_muted);
        self->_mute_callback(self->_muted);
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
