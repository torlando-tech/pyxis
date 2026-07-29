#include "LocationShareScreen.h"
#include "Theme.h"

#ifdef ARDUINO
#include "../LVGL/LVGLLock.h"
#include "../LVGL/LVGLInit.h"
#include <cstring>

namespace UI {
namespace LXMF {

// NO DIRECT SCHEDULER ACCESS CONTRACT: this screen may only publish fixed UI
// commands. It never owns or mutates LocationShareScheduler, persistence, I/O,
// or router state.
namespace {
lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_coord_t width) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_width(button, width);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}
}

LocationShareScreen::LocationShareScreen()
    : screen_(nullptr), back_button_(nullptr), state_label_(nullptr), error_label_(nullptr),
      duration_dropdown_(nullptr), cadence_dropdown_(nullptr),
      precision_dropdown_(nullptr), start_button_(nullptr), stop_button_(nullptr),
      confirmation_dialog_(nullptr) {
    LVGL_LOCK();
    screen_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(screen_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(screen_, Theme::surface(), 0);
    lv_obj_set_style_pad_all(screen_, 6, 0);
    lv_obj_set_flex_flow(screen_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(screen_, 4, 0);

    lv_obj_t* header = lv_obj_create(screen_);
    lv_obj_set_size(header, LV_PCT(100), 34);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(header, 2, 0);
    back_button_ = makeButton(header, LV_SYMBOL_LEFT, 44);
    lv_obj_add_event_cb(back_button_, on_back, LV_EVENT_CLICKED, this);
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, LV_SYMBOL_GPS " Location sharing");
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_flex_grow(title, 1);

    state_label_ = lv_label_create(screen_);
    lv_obj_set_width(state_label_, LV_PCT(100));
    lv_label_set_long_mode(state_label_, LV_LABEL_LONG_WRAP);

    duration_dropdown_ = lv_dropdown_create(screen_);
    lv_dropdown_set_options(duration_dropdown_, "15 min\n1 hour\n4 hours");
    lv_obj_set_width(duration_dropdown_, LV_PCT(100));
    lv_obj_add_event_cb(duration_dropdown_, on_selection, LV_EVENT_VALUE_CHANGED, this);
    cadence_dropdown_ = lv_dropdown_create(screen_);
    lv_dropdown_set_options(cadence_dropdown_, "1 min\n5 min\n15 min");
    lv_obj_set_width(cadence_dropdown_, LV_PCT(100));
    lv_obj_add_event_cb(cadence_dropdown_, on_selection, LV_EVENT_VALUE_CHANGED, this);
    precision_dropdown_ = lv_dropdown_create(screen_);
    lv_dropdown_set_options(precision_dropdown_, "Exact (approximation off)\n100 m\n1 km\n10 km");
    lv_obj_set_width(precision_dropdown_, LV_PCT(100));
    lv_obj_add_event_cb(precision_dropdown_, on_selection, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* actions = lv_obj_create(screen_);
    lv_obj_set_size(actions, LV_PCT(100), 42);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(actions, 2, 0);
    start_button_ = makeButton(actions, "Start / Update", 180);
    lv_obj_add_event_cb(start_button_, on_start, LV_EVENT_CLICKED, this);
    stop_button_ = makeButton(actions, "Stop", 100);
    lv_obj_add_event_cb(stop_button_, on_stop, LV_EVENT_CLICKED, this);

    error_label_ = lv_label_create(screen_);
    lv_obj_set_width(error_label_, LV_PCT(100));
    lv_label_set_long_mode(error_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(error_label_, Theme::error(), 0);
    hide();
}

LocationShareScreen::~LocationShareScreen() {
    hide();
    LVGL_LOCK();
    if (screen_) lv_obj_del(screen_);
}

bool LocationShareScreen::open_for_peer(const uint8_t* peer, std::size_t peer_size,
                                        const Telemetry::ShareSession* session) {
    if (!model_.openForPeer(peer, peer_size)) { refresh(); return false; }
    if (session) {
        if (session->cease_pending) model_.markStopping();
        else model_.applyActive(session->expires_at_millis, session->cadence_millis,
                                session->has_approx_radius, session->approx_radius_meters);
    }
    refresh();
    return true;
}

bool LocationShareScreen::matches_peer(const uint8_t* peer, std::size_t peer_size) const {
    return peer != nullptr && peer_size == 16 && model_.hasPeer() &&
           std::memcmp(model_.peer(), peer, 16) == 0;
}

void LocationShareScreen::apply_result(Telemetry::LocationConsentResult result,
                                       const Telemetry::ShareSession* session) {
    switch (result) {
        case Telemetry::LocationConsentResult::STARTED:
        case Telemetry::LocationConsentResult::UPDATED:
            if (session) model_.applyActive(session->expires_at_millis, session->cadence_millis,
                                            session->has_approx_radius, session->approx_radius_meters);
            else model_.applyError(LocationShareControlError::INVALID);
            break;
        case Telemetry::LocationConsentResult::STOPPING: model_.markStopping(); break;
        case Telemetry::LocationConsentResult::NOT_FOUND: model_.applyOff(); break;
        case Telemetry::LocationConsentResult::CLOCK_UNAVAILABLE:
            model_.applyError(LocationShareControlError::CLOCK_UNAVAILABLE); break;
        case Telemetry::LocationConsentResult::STORAGE_FAILURE:
        case Telemetry::LocationConsentResult::NOT_READY:
            model_.applyError(LocationShareControlError::STORAGE_FAILURE); break;
        case Telemetry::LocationConsentResult::CAPACITY:
            model_.applyError(LocationShareControlError::CAPACITY); break;
        case Telemetry::LocationConsentResult::BUSY:
            model_.applyError(LocationShareControlError::BUSY); break;
        case Telemetry::LocationConsentResult::INVALID_ARGUMENT:
            model_.applyError(LocationShareControlError::INVALID); break;
    }
    refresh();
}

void LocationShareScreen::refresh() {
    char state[160] = {};
    model_.formatState(state, sizeof(state));
    lv_label_set_text(state_label_, state);
    lv_label_set_text(error_label_, model_.errorText());
    const bool canStop = model_.status() == LocationShareControlStatus::ACTIVE;
    if (canStop) lv_obj_clear_state(stop_button_, LV_STATE_DISABLED);
    else lv_obj_add_state(stop_button_, LV_STATE_DISABLED);
}

void LocationShareScreen::show_confirmation() {
    if (!model_.requestConfirmation()) { refresh(); return; }
    char text[192] = {};
    if (!model_.formatConfirmation(text, sizeof(text))) {
        model_.applyError(LocationShareControlError::INVALID); refresh(); return;
    }
    static const char* buttons[] = {"Confirm", "Cancel", ""};
    confirmation_dialog_ = lv_msgbox_create(nullptr, "Confirm location sharing", text,
                                             buttons, false);
    lv_obj_center(confirmation_dialog_);
    lv_obj_add_event_cb(confirmation_dialog_, on_confirmation,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_obj_t* buttons_object = lv_msgbox_get_btns(confirmation_dialog_);
        lv_group_add_obj(group, buttons_object);
        lv_group_focus_obj(buttons_object);
    }
}

void LocationShareScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(screen_);
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_add_obj(group, back_button_);
        lv_group_add_obj(group, duration_dropdown_);
        lv_group_add_obj(group, cadence_dropdown_);
        lv_group_add_obj(group, precision_dropdown_);
        lv_group_add_obj(group, start_button_);
        lv_group_add_obj(group, stop_button_);
        lv_group_focus_obj(back_button_);
    }
}
void LocationShareScreen::hide() {
    LVGL_LOCK();
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_remove_obj(back_button_);
        lv_group_remove_obj(duration_dropdown_);
        lv_group_remove_obj(cadence_dropdown_);
        lv_group_remove_obj(precision_dropdown_);
        lv_group_remove_obj(start_button_);
        lv_group_remove_obj(stop_button_);
        if (confirmation_dialog_) {
            lv_group_remove_obj(lv_msgbox_get_btns(confirmation_dialog_));
        }
    }
    if (confirmation_dialog_) { lv_msgbox_close(confirmation_dialog_); confirmation_dialog_ = nullptr; }
    model_.cancelConfirmation();
    lv_obj_add_flag(screen_, LV_OBJ_FLAG_HIDDEN);
}

void LocationShareScreen::on_back(lv_event_t* event) {
    LocationShareScreen* self = static_cast<LocationShareScreen*>(lv_event_get_user_data(event));
    if (self->back_callback_) self->back_callback_();
}
void LocationShareScreen::on_start(lv_event_t* event) {
    static_cast<LocationShareScreen*>(lv_event_get_user_data(event))->show_confirmation();
}
void LocationShareScreen::on_stop(lv_event_t* event) {
    LocationShareScreen* self = static_cast<LocationShareScreen*>(lv_event_get_user_data(event));
    if (!self->model_.hasPeer() || !self->stop_callback_ ||
        !self->stop_callback_(self->model_.peer(), 16)) {
        self->model_.applyError(LocationShareControlError::BUSY);
    } else {
        self->model_.markStopping();
    }
    self->refresh();
}
void LocationShareScreen::on_selection(lv_event_t* event) {
    LocationShareScreen* self = static_cast<LocationShareScreen*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_target(event);
    const uint16_t selected = lv_dropdown_get_selected(target);
    if (target == self->duration_dropdown_) self->model_.selectDuration(static_cast<LocationShareDuration>(selected));
    else if (target == self->cadence_dropdown_) self->model_.selectCadence(static_cast<LocationShareCadence>(selected));
    else if (target == self->precision_dropdown_) self->model_.selectPrecision(static_cast<LocationSharePrecision>(selected));
}
void LocationShareScreen::on_confirmation(lv_event_t* event) {
    LocationShareScreen* self = static_cast<LocationShareScreen*>(lv_event_get_user_data(event));
    lv_obj_t* dialog = lv_event_get_current_target(event);
    const uint16_t selected = lv_msgbox_get_active_btn(dialog);
    if (selected == 0 && self->start_callback_) {
        const bool queued = self->start_callback_(
            self->model_.peer(), 16, static_cast<uint8_t>(self->model_.duration()),
            self->model_.cadenceMillis(), self->model_.hasApproximation(),
            self->model_.approximationMeters());
        if (!queued) self->model_.applyError(LocationShareControlError::BUSY);
    } else {
        self->model_.cancelConfirmation();
    }
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_remove_obj(lv_msgbox_get_btns(dialog));
        lv_group_focus_obj(self->start_button_);
    }
    self->confirmation_dialog_ = nullptr;
    lv_msgbox_close(dialog);
    self->refresh();
}

} // namespace LXMF
} // namespace UI
#endif
