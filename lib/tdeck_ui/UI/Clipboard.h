#pragma once

#include <Arduino.h>

namespace UI {

/**
 * @brief Simple in-app clipboard for copy/paste functionality
 *
 * Since ESP32 doesn't have a system clipboard, this provides
 * a simple static clipboard for the application.
 */
class Clipboard {
public:
    /**
     * @brief Copy text to clipboard
     * @param text Text to copy
     */
    static void copy(const String& text) {
        _content = text;
        _has_content = true;
    }

    /**
     * @brief Get clipboard content
     * @return Reference to clipboard text (empty if nothing copied)
     */
    static const String& paste() {
        return _content;
    }

    /**
     * @brief Check if clipboard has content
     * @return true if clipboard is not empty
     */
    static bool has_content() {
        return _has_content && _content.length() > 0;
    }

    /**
     * @brief Clear clipboard
     */
    static void clear() {
        _content = "";
        _has_content = false;
    }

private:
    static String _content;
    static bool _has_content;
};

}  // namespace UI
