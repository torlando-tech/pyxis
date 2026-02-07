// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_COMPOSESCREEN_H
#define UI_LXMF_COMPOSESCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <functional>
#include "Bytes.h"

namespace UI {
namespace LXMF {

/**
 * Compose New Message Screen
 *
 * Allows user to compose a message to a new peer by entering:
 * - Destination hash (16 bytes = 32 hex characters)
 * - Message content
 *
 * Layout:
 * ┌─────────────────────────────────────┐
 * │ ← New Message                       │ 32px Header
 * ├─────────────────────────────────────┤
 * │ To:                                 │
 * │ [Paste destination hash - 32 chars]│
 * │                                     │
 * │ Message:                            │
 * │ ┌─────────────────────────────────┐ │ 156px content
 * │ │ [Type your message here...]     │ │
 * │ │                                 │ │
 * │ └─────────────────────────────────┘ │
 * ├─────────────────────────────────────┤
 * │           [Cancel]    [Send]        │ 52px buttons
 * └─────────────────────────────────────┘
 */
class ComposeScreen {
public:
    /**
     * Callback types
     */
    using CancelCallback = std::function<void()>;
    using SendCallback = std::function<void(const RNS::Bytes& dest_hash, const String& message)>;

    /**
     * Create compose screen
     * @param parent Parent LVGL object (usually lv_scr_act())
     */
    ComposeScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~ComposeScreen();

    /**
     * Clear all input fields
     */
    void clear();

    /**
     * Set destination hash (pre-fill the to field)
     * @param dest_hash Destination hash to set
     */
    void set_destination(const RNS::Bytes& dest_hash);

    /**
     * Set callback for cancel button
     * @param callback Function to call when cancel is pressed
     */
    void set_cancel_callback(CancelCallback callback);

    /**
     * Set callback for send button
     * @param callback Function to call when send is pressed
     */
    void set_send_callback(SendCallback callback);

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
     * @return Root object
     */
    lv_obj_t* get_object();

private:
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _content_area;
    lv_obj_t* _button_area;
    lv_obj_t* _text_area_dest;
    lv_obj_t* _text_area_message;
    lv_obj_t* _btn_cancel;
    lv_obj_t* _btn_send;
    lv_obj_t* _btn_back;

    CancelCallback _cancel_callback;
    SendCallback _send_callback;

    // UI construction
    void create_header();
    void create_content_area();
    void create_button_area();

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_cancel_clicked(lv_event_t* event);
    static void on_send_clicked(lv_event_t* event);

    // Validation
    bool validate_destination_hash(const String& hash_str);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_COMPOSESCREEN_H
