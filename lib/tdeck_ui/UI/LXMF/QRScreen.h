// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_QRSCREEN_H
#define UI_LXMF_QRSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <functional>
#include "Bytes.h"
#include "Identity.h"

namespace UI {
namespace LXMF {

/**
 * QR Code Screen
 *
 * Full-screen display of QR code for identity sharing.
 * Shows a large, easily scannable QR code with the LXMF address.
 *
 * Layout:
 * ┌─────────────────────────────────────┐
 * │ ← Share Identity                    │ 36px header
 * ├─────────────────────────────────────┤
 * │                                     │
 * │         ┌───────────────┐           │
 * │         │               │           │
 * │         │   QR CODE     │           │
 * │         │               │           │
 * │         └───────────────┘           │
 * │                                     │
 * │      Scan to add contact            │
 * └─────────────────────────────────────┘
 */
class QRScreen {
public:
    using BackCallback = std::function<void()>;

    /**
     * Create QR screen
     * @param parent Parent LVGL object
     */
    QRScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~QRScreen();

    /**
     * Set identity for QR code generation
     * @param identity The identity
     */
    void set_identity(const RNS::Identity& identity);

    /**
     * Set LXMF delivery destination hash
     * @param hash The delivery destination hash
     */
    void set_lxmf_address(const RNS::Bytes& hash);

    /**
     * Set callback for back button
     * @param callback Function to call when back is pressed
     */
    void set_back_callback(BackCallback callback);

    /**
     * Show the screen
     */
    void show();

    /**
     * Hide the screen
     */
    void hide();

    /**
     * Get the root LVGL object
     */
    lv_obj_t* get_object();

private:
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _content;
    lv_obj_t* _btn_back;
    lv_obj_t* _qr_code;
    lv_obj_t* _label_hint;

    RNS::Identity _identity;
    RNS::Bytes _lxmf_address;

    BackCallback _back_callback;

    void create_header();
    void create_content();
    void update_qr_code();

    static void on_back_clicked(lv_event_t* event);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_QRSCREEN_H
