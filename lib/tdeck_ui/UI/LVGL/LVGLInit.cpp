// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "LVGLInit.h"

#ifdef ARDUINO

#include "esp_task_wdt.h"

#include "Log.h"
#include "../../Hardware/TDeck/Display.h"
#include "../../Hardware/TDeck/Keyboard.h"
#include "../../Hardware/TDeck/Touch.h"
#include "../../Hardware/TDeck/Trackball.h"

using namespace RNS;
using namespace Hardware::TDeck;

namespace UI {
namespace LVGL {

bool LVGLInit::_initialized = false;
lv_disp_t* LVGLInit::_display = nullptr;
lv_indev_t* LVGLInit::_keyboard = nullptr;
lv_indev_t* LVGLInit::_touch = nullptr;
lv_indev_t* LVGLInit::_trackball = nullptr;
lv_group_t* LVGLInit::_default_group = nullptr;
TaskHandle_t LVGLInit::_task_handle = nullptr;
SemaphoreHandle_t LVGLInit::_mutex = nullptr;

bool LVGLInit::init() {
    if (_initialized) {
        return true;
    }

    INFO("Initializing LVGL");

    // Create recursive mutex for thread-safe LVGL access
    // Recursive because LVGL callbacks may call other LVGL functions
    _mutex = xSemaphoreCreateRecursiveMutex();
    if (!_mutex) {
        ERROR("Failed to create LVGL mutex");
        return false;
    }

    // Initialize LVGL library
    lv_init();

    // LVGL 8.x logging is configured via lv_conf.h LV_USE_LOG
    // No runtime callback registration needed

    // Initialize display (this also sets up LVGL display driver)
    if (!Display::init()) {
        ERROR("Failed to initialize display for LVGL");
        return false;
    }
    _display = lv_disp_get_default();

    INFO("  Display initialized");

    // Create default input group for keyboard navigation
    _default_group = lv_group_create();
    if (!_default_group) {
        ERROR("Failed to create input group");
        return false;
    }
    lv_group_set_default(_default_group);

    // Initialize keyboard input
    if (Keyboard::init()) {
        _keyboard = Keyboard::get_indev();
        // Associate keyboard with input group
        if (_keyboard) {
            lv_indev_set_group(_keyboard, _default_group);
            INFO("  Keyboard registered with input group");
        }
    } else {
        WARNING("  Keyboard initialization failed");
    }

    // Initialize touch input
    if (Touch::init()) {
        _touch = lv_indev_get_next(_keyboard);
        INFO("  Touch registered");
    } else {
        WARNING("  Touch initialization failed");
    }

    // Initialize trackball input
    if (Trackball::init()) {
        _trackball = Trackball::get_indev();
        // Associate trackball with input group for focus navigation
        if (_trackball) {
            lv_indev_set_group(_trackball, _default_group);
            INFO("  Trackball registered with input group");
        }
    } else {
        WARNING("  Trackball initialization failed");
    }

    // Set default dark theme
    set_theme(true);

    _initialized = true;
    INFO("LVGL initialized successfully");

    return true;
}

bool LVGLInit::init_display_only() {
    if (_initialized) {
        return true;
    }

    INFO("Initializing LVGL (display only)");

    // Initialize LVGL library
    lv_init();

    // LVGL 8.x logging is configured via lv_conf.h LV_USE_LOG
    // No runtime callback registration needed

    // Initialize display
    if (!Display::init()) {
        ERROR("Failed to initialize display for LVGL");
        return false;
    }
    _display = lv_disp_get_default();

    INFO("  Display initialized");

    // Set default dark theme
    set_theme(true);

    _initialized = true;
    INFO("LVGL initialized (display only)");

    return true;
}

void LVGLInit::task_handler() {
    if (!_initialized) {
        return;
    }

    // If running as a task, this is a no-op
    if (_task_handle != nullptr) {
        return;
    }

    lv_task_handler();
}

void LVGLInit::lvgl_task(void* param) {
    Serial.printf("LVGL task started on core %d\n", xPortGetCoreID());

    // Subscribe this task to Task Watchdog Timer
    esp_task_wdt_add(nullptr);  // nullptr = current task

    while (true) {
        // Acquire mutex before calling LVGL
#ifndef NDEBUG
        // Debug builds: 5-second timeout for stuck task detection
        BaseType_t result = xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(5000));
        if (result != pdTRUE) {
            WARNING("LVGL task mutex timeout (5s) - possible deadlock or stuck task");
            // Per Phase 8 context: log warning and continue waiting (don't break functionality)
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        }
#else
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
#endif
        lv_task_handler();
        xSemaphoreGiveRecursive(_mutex);

        // Feed watchdog and yield to other tasks
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool LVGLInit::start_task(int priority, int core) {
    if (!_initialized) {
        ERROR("Cannot start LVGL task - not initialized");
        return false;
    }

    if (_task_handle != nullptr) {
        WARNING("LVGL task already running");
        return true;
    }

    // Create the task
    BaseType_t result;
    if (core >= 0) {
        result = xTaskCreatePinnedToCore(
            lvgl_task,
            "lvgl",
            8192,           // Stack size (8KB should be enough for LVGL)
            nullptr,
            priority,
            &_task_handle,
            core
        );
    } else {
        result = xTaskCreate(
            lvgl_task,
            "lvgl",
            8192,
            nullptr,
            priority,
            &_task_handle
        );
    }

    if (result != pdPASS) {
        ERROR("Failed to create LVGL task");
        return false;
    }

    Serial.printf("LVGL task created with priority %d%s%d\n",
                   priority,
                   core >= 0 ? " on core " : "",
                   core >= 0 ? core : 0);
    return true;
}

bool LVGLInit::is_task_running() {
    return _task_handle != nullptr;
}

SemaphoreHandle_t LVGLInit::get_mutex() {
    return _mutex;
}

uint32_t LVGLInit::get_tick() {
    return millis();
}

bool LVGLInit::is_initialized() {
    return _initialized;
}

void LVGLInit::set_theme(bool dark) {
    if (!_initialized) {
        return;
    }

    lv_theme_t* theme;

    if (dark) {
        // Dark theme with blue accents
        theme = lv_theme_default_init(
            _display,
            lv_palette_main(LV_PALETTE_BLUE),      // Primary color
            lv_palette_main(LV_PALETTE_RED),       // Secondary color
            true,                                    // Dark mode
            &lv_font_montserrat_14                  // Default font
        );
    } else {
        // Light theme
        theme = lv_theme_default_init(
            _display,
            lv_palette_main(LV_PALETTE_BLUE),
            lv_palette_main(LV_PALETTE_RED),
            false,                                   // Light mode
            &lv_font_montserrat_14
        );
    }

    lv_disp_set_theme(_display, theme);
}

lv_disp_t* LVGLInit::get_display() {
    return _display;
}

lv_indev_t* LVGLInit::get_keyboard() {
    return _keyboard;
}

lv_indev_t* LVGLInit::get_touch() {
    return _touch;
}

lv_indev_t* LVGLInit::get_trackball() {
    return _trackball;
}

lv_group_t* LVGLInit::get_default_group() {
    return _default_group;
}

void LVGLInit::focus_widget(lv_obj_t* obj) {
    if (!_default_group || !obj) {
        return;
    }

    // Remove from group first if already there (to avoid duplicates)
    lv_group_remove_obj(obj);

    // Add to group and focus
    lv_group_add_obj(_default_group, obj);
    lv_group_focus_obj(obj);
}

void LVGLInit::log_print(const char* buf) {
    // Forward LVGL logs to our logging system
    // LVGL logs include newlines, so strip them
    String msg(buf);
    msg.trim();

    if (msg.length() > 0) {
        // LVGL log levels: Trace, Info, Warn, Error
        if (msg.indexOf("[Error]") >= 0) {
            ERROR(msg.c_str());
        } else if (msg.indexOf("[Warn]") >= 0) {
            WARNING(msg.c_str());
        } else if (msg.indexOf("[Info]") >= 0) {
            INFO(msg.c_str());
        } else {
            TRACE(msg.c_str());
        }
    }
}

} // namespace LVGL
} // namespace UI

#endif // ARDUINO
