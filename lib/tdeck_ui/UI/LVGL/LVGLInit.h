// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LVGL_LVGLINIT_H
#define UI_LVGL_LVGLINIT_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>

namespace UI {
namespace LVGL {

/**
 * LVGL Initialization and Configuration
 *
 * Handles:
 * - LVGL library initialization
 * - Display driver integration
 * - Input device integration (keyboard, touch, trackball)
 * - Theme configuration
 * - Memory management
 *
 * Must be called after hardware drivers are initialized.
 */
class LVGLInit {
public:
    /**
     * Initialize LVGL with all input devices
     * Requires Display, Keyboard, Touch, and Trackball to be initialized first
     * @return true if initialization successful
     */
    static bool init();

    /**
     * Initialize LVGL with minimal setup (display only)
     * @return true if initialization successful
     */
    static bool init_display_only();

    /**
     * Task handler - must be called periodically (e.g., in main loop)
     * Handles LVGL rendering and input processing
     * NOTE: If start_task() was called, this is a no-op as LVGL runs on its own task
     */
    static void task_handler();

    /**
     * Start LVGL on its own FreeRTOS task
     * This allows LVGL to run independently of the main loop, preventing
     * UI freezes when other operations (like BLE) take time.
     * @param priority Task priority (default 1, higher than idle)
     * @param core Core to pin the task to (-1 for no affinity, 0 or 1 for specific core)
     * @return true if task started successfully
     */
    static bool start_task(int priority = 1, int core = 1);

    /**
     * Check if LVGL is running on its own task
     * @return true if running as a FreeRTOS task
     */
    static bool is_task_running();

    /**
     * Get the LVGL mutex for thread-safe access
     * All LVGL API calls from outside the LVGL task must acquire this mutex.
     * @return Recursive mutex handle
     */
    static SemaphoreHandle_t get_mutex();

    /**
     * Get the LVGL FreeRTOS task handle
     * Useful for stack monitoring and task introspection.
     * @return Task handle, or nullptr if task not started
     */
    static TaskHandle_t get_task_handle() { return _task_handle; }

    /**
     * Get time in milliseconds for LVGL
     * Required LVGL callback
     */
    static uint32_t get_tick();

    /**
     * Check if LVGL is initialized
     * @return true if initialized
     */
    static bool is_initialized();

    /**
     * Set default theme (dark or light)
     * @param dark true for dark theme, false for light theme
     */
    static void set_theme(bool dark = true);

    /**
     * Get current LVGL display object
     * @return LVGL display object, or nullptr if not initialized
     */
    static lv_disp_t* get_display();

    /**
     * Get keyboard input device
     * @return LVGL input device, or nullptr if not initialized
     */
    static lv_indev_t* get_keyboard();

    /**
     * Get touch input device
     * @return LVGL input device, or nullptr if not initialized
     */
    static lv_indev_t* get_touch();

    /**
     * Get trackball input device
     * @return LVGL input device, or nullptr if not initialized
     */
    static lv_indev_t* get_trackball();

    /**
     * Get the default input group for keyboard/encoder navigation
     * @return LVGL group object
     */
    static lv_group_t* get_default_group();

    /**
     * Focus a widget (add to group and set as focused)
     * @param obj Widget to focus
     */
    static void focus_widget(lv_obj_t* obj);

private:
    static bool _initialized;
    static lv_disp_t* _display;
    static lv_indev_t* _keyboard;
    static lv_indev_t* _touch;
    static lv_indev_t* _trackball;
    static lv_group_t* _default_group;

    // FreeRTOS task support
    static TaskHandle_t _task_handle;
    static SemaphoreHandle_t _mutex;
    static void lvgl_task(void* param);

    // LVGL logging callback
    static void log_print(const char* buf);
};

} // namespace LVGL
} // namespace UI

#endif // ARDUINO
#endif // UI_LVGL_LVGLINIT_H
