// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "Trackball.h"
#include "TrackballNavigation.h"

#ifdef ARDUINO

#include <microReticulum/Log.h>
#include <driver/gpio.h>
#include <limits>

// Defined in main.cpp; mirrors input diagnostics to Serial and UDP logging.
extern "C" void pyxis_log(const char* msg);

using namespace RNS;

namespace Hardware {
namespace TDeck {

namespace {

constexpr lv_coord_t TRACKBALL_SCROLL_STEP = 40;

bool object_is_visible(lv_obj_t* object) {
    return object && lv_obj_is_visible(object);
}

bool object_is_focus_candidate(lv_obj_t* object) {
    return object_is_visible(object) &&
           !lv_obj_has_state(object, LV_STATE_DISABLED);
}

bool object_is_hidden(lv_obj_t* object) {
    for (lv_obj_t* current = object; current; current = lv_obj_get_parent(current)) {
        if (lv_obj_has_flag(current, LV_OBJ_FLAG_HIDDEN)) return true;
    }
    return false;
}

NavigationRect object_rect(lv_obj_t* object) {
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    return {area.x1, area.y1, lv_area_get_width(&area), lv_area_get_height(&area)};
}

bool is_descendant_of(lv_obj_t* object, lv_obj_t* ancestor) {
    for (lv_obj_t* current = object; current; current = lv_obj_get_parent(current)) {
        if (current == ancestor) return true;
    }
    return false;
}

lv_obj_t* common_ancestor(lv_obj_t* first, lv_obj_t* second) {
    if (!first) return second;
    if (!second) return first;
    for (lv_obj_t* candidate = first; candidate; candidate = lv_obj_get_parent(candidate)) {
        if (is_descendant_of(second, candidate)) return candidate;
    }
    return nullptr;
}

lv_obj_t* group_navigation_root(lv_group_t* group, lv_obj_t* focused) {
    lv_obj_t* root = focused;
    lv_obj_t** node = static_cast<lv_obj_t**>(_lv_ll_get_head(&group->obj_ll));
    while (node) {
        lv_obj_t* object = *node;
        if (object_is_visible(object)) root = common_ancestor(root, object);
        node = static_cast<lv_obj_t**>(_lv_ll_get_next(&group->obj_ll, node));
    }
    return root;
}

bool can_scroll(lv_obj_t* object, NavigationDirection direction) {
    if (!object || !object_is_visible(object) ||
        !lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE)) return false;
    const lv_dir_t scroll_dir = lv_obj_get_scroll_dir(object);
    switch (direction) {
        case NavigationDirection::UP:
            return (scroll_dir & LV_DIR_VER) && lv_obj_get_scroll_top(object) > 0;
        case NavigationDirection::DOWN:
            return (scroll_dir & LV_DIR_VER) && lv_obj_get_scroll_bottom(object) > 0;
        case NavigationDirection::LEFT:
            return (scroll_dir & LV_DIR_HOR) && lv_obj_get_scroll_left(object) > 0;
        case NavigationDirection::RIGHT:
            return (scroll_dir & LV_DIR_HOR) && lv_obj_get_scroll_right(object) > 0;
    }
    return false;
}

lv_obj_t* scrollable_ancestor(lv_obj_t* object, NavigationDirection direction) {
    for (lv_obj_t* current = object; current; current = lv_obj_get_parent(current)) {
        if (can_scroll(current, direction)) return current;
    }
    return nullptr;
}

void find_scroll_target(lv_obj_t* root, NavigationDirection direction,
                        lv_coord_t perpendicular_position,
                        lv_obj_t*& best, int32_t& best_area) {
    if (!root || !object_is_visible(root)) return;
    if (can_scroll(root, direction)) {
        const NavigationRect rect = object_rect(root);
        const bool intersects = direction == NavigationDirection::UP ||
                                direction == NavigationDirection::DOWN
            ? perpendicular_position >= rect.x && perpendicular_position < rect.x + rect.width
            : perpendicular_position >= rect.y && perpendicular_position < rect.y + rect.height;
        const int32_t area = rect.width * rect.height;
        if (intersects && area < best_area) {
            best = root;
            best_area = area;
        }
    }
    const uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < child_count; ++i) {
        find_scroll_target(lv_obj_get_child(root, i), direction,
                           perpendicular_position, best, best_area);
    }
}

void scroll_one_step(lv_obj_t* object, NavigationDirection direction) {
    switch (direction) {
        case NavigationDirection::UP:
            lv_obj_scroll_to_y(object, lv_obj_get_scroll_y(object) - TRACKBALL_SCROLL_STEP, LV_ANIM_ON);
            break;
        case NavigationDirection::DOWN:
            lv_obj_scroll_to_y(object, lv_obj_get_scroll_y(object) + TRACKBALL_SCROLL_STEP, LV_ANIM_ON);
            break;
        case NavigationDirection::LEFT:
            lv_obj_scroll_to_x(object, lv_obj_get_scroll_x(object) - TRACKBALL_SCROLL_STEP, LV_ANIM_ON);
            break;
        case NavigationDirection::RIGHT:
            lv_obj_scroll_to_x(object, lv_obj_get_scroll_x(object) + TRACKBALL_SCROLL_STEP, LV_ANIM_ON);
            break;
    }
}

bool navigate_or_scroll(lv_group_t* group, NavigationDirection direction) {
    if (!group) return false;
    if (group->frozen) return false;
    lv_obj_t* focused = lv_group_get_focused(group);
    // A clipped focus target remains the spatial anchor while its container
    // scrolls. Only a truly hidden/stale target needs insertion-order refocus.
    if (!focused || object_is_hidden(focused)) {
        lv_obj_t** node = static_cast<lv_obj_t**>(_lv_ll_get_head(&group->obj_ll));
        while (node) {
            if (object_is_focus_candidate(*node)) {
                lv_group_focus_obj(*node);
                return true;
            }
            node = static_cast<lv_obj_t**>(_lv_ll_get_next(&group->obj_ll, node));
        }
        return false;
    }

    const NavigationRect focused_rect = object_rect(focused);
    lv_obj_t* navigation_root = group_navigation_root(group, focused);
    lv_obj_t* target = nullptr;
    int64_t best_score = std::numeric_limits<int64_t>::max();
    lv_obj_t** node = static_cast<lv_obj_t**>(_lv_ll_get_head(&group->obj_ll));
    while (node) {
        lv_obj_t* object = *node;
        if (object != focused) {
            const NavigationCandidate candidate{0, object_rect(object), object_is_focus_candidate(object)};
            const int64_t score = directional_candidate_score(focused_rect, candidate, direction);
            if (score < best_score) {
                best_score = score;
                target = object;
            }
        }
        node = static_cast<lv_obj_t**>(_lv_ll_get_next(&group->obj_ll, node));
    }

    lv_obj_t* ancestor = scrollable_ancestor(focused, direction);
    if (target) {
        // Finish scrolling the current list/document before leaving it for a
        // control outside that viewport. Targets inside it still navigate.
        if (ancestor && !is_descendant_of(target, ancestor)) {
            scroll_one_step(ancestor, direction);
        } else {
            lv_group_focus_obj(target);
        }
        return true;
    }
    if (ancestor) {
        scroll_one_step(ancestor, direction);
        return true;
    }

    lv_obj_t* scroll_target = nullptr;
    int32_t best_area = INT32_MAX;
    const lv_coord_t perpendicular = direction == NavigationDirection::UP ||
                                     direction == NavigationDirection::DOWN
        ? focused_rect.x + focused_rect.width / 2
        : focused_rect.y + focused_rect.height / 2;
    find_scroll_target(navigation_root, direction, perpendicular, scroll_target, best_area);
    if (scroll_target) {
        scroll_one_step(scroll_target, direction);
        return true;
    }
    return false;
}

uint32_t direction_key(NavigationDirection direction) {
    switch (direction) {
        case NavigationDirection::UP: return LV_KEY_UP;
        case NavigationDirection::DOWN: return LV_KEY_DOWN;
        case NavigationDirection::LEFT: return LV_KEY_LEFT;
        case NavigationDirection::RIGHT: return LV_KEY_RIGHT;
    }
    return 0;
}

} // namespace

// Static member initialization
lv_indev_t* Trackball::_indev = nullptr;
volatile int16_t Trackball::_pulse_up = 0;
volatile int16_t Trackball::_pulse_down = 0;
volatile int16_t Trackball::_pulse_left = 0;
volatile int16_t Trackball::_pulse_right = 0;
volatile uint32_t Trackball::_last_pulse_time = 0;

ButtonDebouncer Trackball::_button_debouncer(Trk::DEBOUNCE_MS);
Trackball::State Trackball::_state;
bool Trackball::_initialized = false;

bool Trackball::init() {
    if (_initialized) {
        return true;
    }

    INFO("Initializing T-Deck trackball");

    // Initialize hardware first
    if (!init_hardware_only()) {
        return false;
    }

    // Register LVGL input device as KEYPAD for focus navigation
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_KEYPAD;  // KEYPAD for 2D focus navigation
    indev_drv.read_cb = lvgl_read_cb;

    _indev = lv_indev_drv_register(&indev_drv);
    if (!_indev) {
        ERROR("Failed to register trackball with LVGL");
        return false;
    }

    INFO("Trackball initialized successfully");
    return true;
}

bool Trackball::init_hardware_only() {
    if (_initialized) {
        return true;
    }

    INFO("Initializing trackball hardware");

    // Use ESP-IDF gpio driver for reliable interrupt handling on strapping pins
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;  // Falling edge trigger
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    // Configure all trackball directional pins
    io_conf.pin_bit_mask = (1ULL << Pin::TRACKBALL_UP) |
                           (1ULL << Pin::TRACKBALL_DOWN) |
                           (1ULL << Pin::TRACKBALL_LEFT) |
                           (1ULL << Pin::TRACKBALL_RIGHT);
    gpio_config(&io_conf);

    // Configure button separately (just input with pullup, no interrupt)
    gpio_config_t btn_conf = {};
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_conf.pin_bit_mask = (1ULL << Pin::TRACKBALL_BUTTON);
    gpio_config(&btn_conf);

    // Install GPIO ISR service
    gpio_install_isr_service(0);

    // Attach ISR handlers
    gpio_isr_handler_add((gpio_num_t)Pin::TRACKBALL_UP, isr_up, nullptr);
    gpio_isr_handler_add((gpio_num_t)Pin::TRACKBALL_DOWN, isr_down, nullptr);
    gpio_isr_handler_add((gpio_num_t)Pin::TRACKBALL_LEFT, isr_left, nullptr);
    gpio_isr_handler_add((gpio_num_t)Pin::TRACKBALL_RIGHT, isr_right, nullptr);

    // Initialize state
    _state.delta_x = 0;
    _state.delta_y = 0;
    _state.button_pressed = _button_debouncer.update(
        digitalRead(Pin::TRACKBALL_BUTTON) == LOW, millis());
    _state.timestamp = millis();

    _initialized = true;
    INFO("  Trackball hardware ready");
    return true;
}

bool Trackball::poll() {
    if (!_initialized) {
        return false;
    }

    bool state_changed = false;
    uint32_t now = millis();

    // Read pulse counters (critical section to avoid race with ISRs)
    noInterrupts();
    int16_t up = _pulse_up;
    int16_t down = _pulse_down;
    int16_t left = _pulse_left;
    int16_t right = _pulse_right;
    uint32_t last_pulse = _last_pulse_time;
    interrupts();


    // Calculate net movement
    int16_t delta_y = down - up;  // Positive = down, negative = up
    int16_t delta_x = right - left;  // Positive = right, negative = left

    // Apply sensitivity multiplier
    delta_x *= Trk::PIXELS_PER_PULSE;
    delta_y *= Trk::PIXELS_PER_PULSE;

    // Update state if movement detected
    if (delta_x != 0 || delta_y != 0) {
        _state.delta_x = delta_x;
        _state.delta_y = delta_y;
        _state.timestamp = now;
        state_changed = true;

        // Reset pulse counters after reading
        noInterrupts();
        _pulse_up = 0;
        _pulse_down = 0;
        _pulse_left = 0;
        _pulse_right = 0;
        interrupts();
    } else {
        // Reset deltas if no recent pulses (timeout)
        if (now - last_pulse > Trk::PULSE_RESET_MS) {
            if (_state.delta_x != 0 || _state.delta_y != 0) {
                _state.delta_x = 0;
                _state.delta_y = 0;
                state_changed = true;
            }
        }
    }

    // Read button state with debouncing
    bool button = read_button_debounced();
    if (button != _state.button_pressed) {
        _state.button_pressed = button;
        state_changed = true;
    }

    return state_changed;
}

void Trackball::get_state(State& state) {
    state = _state;
}

void Trackball::reset_deltas() {
    _state.delta_x = 0;
    _state.delta_y = 0;
}

bool Trackball::is_button_pressed() {
    return _state.button_pressed;
}

lv_indev_t* Trackball::get_indev() {
    return _indev;
}

void Trackball::lvgl_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    // Static accumulators for threshold-based navigation
    static int16_t accum_x = 0;
    static int16_t accum_y = 0;
    static uint32_t last_key_time = 0;
    // Key press/release state machine
    static uint32_t pending_key = 0;      // Key waiting to be pressed
    static uint32_t pressed_key = 0;      // Key currently pressed (needs release)
    static bool button_was_pressed = false;  // Track physical button state for release
    // Poll for new trackball data
    poll();

    // Get current state
    State state;
    get_state(state);

    // Accumulate movement (convert back from pixels to pulses)
    accum_x += state.delta_x / Trk::PIXELS_PER_PULSE;
    accum_y += state.delta_y / Trk::PIXELS_PER_PULSE;

    // Button handling - trigger on release only to avoid double-activation
    // When screen changes on press, release would hit new screen's focused element
    if (state.button_pressed) {
        button_was_pressed = true;
        // Don't send anything yet, wait for release
    } else if (button_was_pressed) {
        // Button just released - queue ENTER key for press/release cycle
        button_was_pressed = false;
        pending_key = LV_KEY_ENTER;
        pyxis_log("[INPUT] trackball enter=release");
        // Fall through to let pending_key logic handle it
    }

    // Check if we need to release a previously pressed navigation key
    if (pressed_key != 0) {
        data->key = pressed_key;
        data->state = LV_INDEV_STATE_RELEASED;
        pressed_key = 0;
        reset_deltas();
        return;
    }

    // Check if we have a pending key to press
    if (pending_key != 0) {
        data->key = pending_key;
        data->state = LV_INDEV_STATE_PRESSED;
        pressed_key = pending_key;  // Mark for release on next callback
        pending_key = 0;
        reset_deltas();
        return;
    }

    uint32_t now = millis();

    // Resolve physical direction independently on both axes. In navigation
    // mode this uses object geometry instead of the group's insertion order;
    // in edit mode real arrow keys are delivered to the focused widget.
    NavigationDirection direction = NavigationDirection::UP;
    bool has_direction = false;
    if (abs(accum_y) >= Trk::NAV_THRESHOLD && abs(accum_y) >= abs(accum_x)) {
        if (now - last_key_time >= Trk::KEY_REPEAT_MS) {
            direction = accum_y > 0 ? NavigationDirection::DOWN : NavigationDirection::UP;
            has_direction = true;
            last_key_time = now;
        }
        accum_y = 0;
    } else if (abs(accum_x) >= Trk::NAV_THRESHOLD) {
        if (now - last_key_time >= Trk::KEY_REPEAT_MS) {
            direction = accum_x > 0 ? NavigationDirection::RIGHT : NavigationDirection::LEFT;
            has_direction = true;
            last_key_time = now;
        }
        accum_x = 0;
    }

    if (has_direction) {
        lv_group_t* group = _indev ? _indev->group : nullptr;
        if (group && lv_group_get_editing(group)) {
            pending_key = direction_key(direction);
        } else {
            navigate_or_scroll(group, direction);
        }
    }

    // If we have a pending key, check if anything visible is focused
    // If nothing is focused or focused object is hidden, find a visible object
    if (pending_key != 0) {
        lv_group_t* group = _indev ? _indev->group : nullptr;
        if (group) {
            lv_obj_t* focused = lv_group_get_focused(group);
            bool need_refocus = !focused;

            // Check if focused object or any parent is hidden
            if (focused && !need_refocus) {
                lv_obj_t* obj = focused;
                while (obj) {
                    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                        need_refocus = true;
                        break;
                    }
                    obj = lv_obj_get_parent(obj);
                }
            }

            if (need_refocus) {
                // Find first visible object in group
                uint32_t obj_cnt = lv_group_get_obj_count(group);
                for (uint32_t i = 0; i < obj_cnt; i++) {
                    lv_group_focus_next(group);
                    lv_obj_t* candidate = lv_group_get_focused(group);
                    if (candidate) {
                        bool visible = true;
                        lv_obj_t* obj = candidate;
                        while (obj) {
                            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                                visible = false;
                                break;
                            }
                            obj = lv_obj_get_parent(obj);
                        }
                        if (visible) break;  // Found a visible object
                    }
                }
                pending_key = 0;  // Don't send the key, just focus
                accum_x = 0;
                accum_y = 0;
            }
        }
    }

    // Default: no key activity
    data->key = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    reset_deltas();
}

bool Trackball::read_button_debounced() {
    const bool raw_pressed = (digitalRead(Pin::TRACKBALL_BUTTON) == LOW);
    const bool stable_pressed = _button_debouncer.update(raw_pressed, millis());
    if (_button_debouncer.changed()) {
        pyxis_log(stable_pressed
            ? "[INPUT] trackball button=pressed"
            : "[INPUT] trackball button=released");
    }
    return stable_pressed;
}

// ISR handlers - MUST be in IRAM for ESP32
// ESP-IDF gpio_isr_handler signature requires void* arg
void IRAM_ATTR Trackball::isr_up(void* arg) {
    _pulse_up++;
    _last_pulse_time = millis();
}

void IRAM_ATTR Trackball::isr_down(void* arg) {
    _pulse_down++;
    _last_pulse_time = millis();
}

void IRAM_ATTR Trackball::isr_left(void* arg) {
    _pulse_left++;
    _last_pulse_time = millis();
}

void IRAM_ATTR Trackball::isr_right(void* arg) {
    _pulse_right++;
    _last_pulse_time = millis();
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
