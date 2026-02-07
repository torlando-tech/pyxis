// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include "Clipboard.h"

namespace UI {

/**
 * @brief Helper for adding paste functionality to any LVGL text area
 *
 * Usage:
 *   lv_obj_t* ta = lv_textarea_create(parent);
 *   TextAreaHelper::enable_paste(ta);
 */
class TextAreaHelper {
public:
    /**
     * @brief Enable paste-on-long-press for a text area
     * @param textarea LVGL textarea object
     *
     * When the user long-presses the textarea, a paste dialog
     * will appear if the clipboard has content.
     */
    static void enable_paste(lv_obj_t* textarea) {
        lv_obj_add_event_cb(textarea, on_long_pressed, LV_EVENT_LONG_PRESSED, nullptr);
    }

private:
    static void on_long_pressed(lv_event_t* event) {
        lv_obj_t* textarea = lv_event_get_target(event);

        // Only show paste if clipboard has content
        if (!Clipboard::has_content()) {
            return;
        }

        // Store textarea pointer in msgbox user data for the callback
        static const char* btns[] = {"Paste", "Cancel", ""};
        lv_obj_t* mbox = lv_msgbox_create(NULL, "Paste",
            "Paste from clipboard?", btns, false);
        lv_obj_center(mbox);
        lv_obj_set_user_data(mbox, textarea);
        lv_obj_add_event_cb(mbox, on_paste_action, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    static void on_paste_action(lv_event_t* event) {
        lv_obj_t* mbox = lv_event_get_current_target(event);
        lv_obj_t* textarea = (lv_obj_t*)lv_obj_get_user_data(mbox);

        uint16_t btn_id = lv_msgbox_get_active_btn(mbox);

        if (btn_id == 0 && textarea) {  // Paste button
            const String& content = Clipboard::paste();
            if (content.length() > 0) {
                lv_textarea_add_text(textarea, content.c_str());
            }
        }

        lv_msgbox_close(mbox);
    }
};

}  // namespace UI

#endif // ARDUINO
