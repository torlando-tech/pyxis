// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "QRScreen.h"

#ifdef ARDUINO

#include "Log.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"

using namespace RNS;

namespace UI {
namespace LXMF {

QRScreen::QRScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _content(nullptr), _btn_back(nullptr),
      _qr_code(nullptr), _label_hint(nullptr) {

    // Create screen object
    if (parent) {
        _screen = lv_obj_create(parent);
    } else {
        _screen = lv_obj_create(lv_scr_act());
    }

    lv_obj_set_size(_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_screen, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_radius(_screen, 0, 0);

    // Create UI components
    create_header();
    create_content();

    // Hide by default
    hide();

    TRACE("QRScreen created");
}

QRScreen::~QRScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void QRScreen::create_header() {
    _header = lv_obj_create(_screen);
    lv_obj_set_size(_header, LV_PCT(100), 36);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);

    // Back button
    _btn_back = lv_btn_create(_header);
    lv_obj_set_size(_btn_back, 50, 28);
    lv_obj_align(_btn_back, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x444444), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_back, on_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_back = lv_label_create(_btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xe0e0e0), 0);

    // Title
    lv_obj_t* title = lv_label_create(_header);
    lv_label_set_text(title, "Share Identity");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
}

void QRScreen::create_content() {
    _content = lv_obj_create(_screen);
    lv_obj_set_size(_content, LV_PCT(100), 204);  // 240 - 36 header
    lv_obj_align(_content, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_content, 8, 0);
    lv_obj_set_style_bg_color(_content, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(_content, 0, 0);
    lv_obj_set_style_radius(_content, 0, 0);
    lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);

    // QR code - centered, large for easy scanning
    // 180px gives ~3.4px per module for 53-module QR (Version 9)
    _qr_code = lv_qrcode_create(_content, 180, lv_color_hex(0x000000), lv_color_hex(0xffffff));
    lv_obj_align(_qr_code, LV_ALIGN_TOP_MID, 0, 0);

    // Hint text below QR
    _label_hint = lv_label_create(_content);
    lv_label_set_text(_label_hint, "Scan with Columba to add contact");
    lv_obj_align(_label_hint, LV_ALIGN_TOP_MID, 0, 183);  // Below 180px QR + 3px gap
    lv_obj_set_style_text_color(_label_hint, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(_label_hint, &lv_font_montserrat_12, 0);
}

void QRScreen::set_identity(const Identity& identity) {
    LVGL_LOCK();
    _identity = identity;
    update_qr_code();
}

void QRScreen::set_lxmf_address(const Bytes& hash) {
    LVGL_LOCK();
    _lxmf_address = hash;
    update_qr_code();
}

void QRScreen::update_qr_code() {
    if (!_identity || !_qr_code || _lxmf_address.size() == 0) {
        return;
    }

    // Format: lxma://<dest_hash>:<public_key>
    // Columba-compatible format for contact sharing
    String dest_hash = String(_lxmf_address.toHex().c_str());
    String pub_key = String(_identity.get_public_key().toHex().c_str());

    String qr_data = "lxma://";
    qr_data += dest_hash;
    qr_data += ":";
    qr_data += pub_key;

    lv_qrcode_update(_qr_code, qr_data.c_str(), qr_data.length());
}

void QRScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void QRScreen::show() {
    LVGL_LOCK();
    update_qr_code();  // Refresh QR when shown
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    // Add back button to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group && _btn_back) {
        lv_group_add_obj(group, _btn_back);
        lv_group_focus_obj(_btn_back);
    }
}

void QRScreen::hide() {
    LVGL_LOCK();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group && _btn_back) {
        lv_group_remove_obj(_btn_back);
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* QRScreen::get_object() {
    return _screen;
}

void QRScreen::on_back_clicked(lv_event_t* event) {
    QRScreen* screen = (QRScreen*)lv_event_get_user_data(event);

    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
