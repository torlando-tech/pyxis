#ifndef PYXIS_UI_LXMF_LOCATION_SHARE_SCREEN_H
#define PYXIS_UI_LXMF_LOCATION_SHARE_SCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <functional>

#include "LocationShareControlModel.h"
#include "Telemetry/LocationPersistenceController.h"

namespace UI {
namespace LXMF {

class LocationShareScreen {
public:
    using BackCallback = std::function<void()>;
    using StartCallback = std::function<bool(const uint8_t*, std::size_t, uint8_t,
                                              uint32_t, bool, int32_t)>;
    using StopCallback = std::function<bool(const uint8_t*, std::size_t)>;

    LocationShareScreen();
    ~LocationShareScreen();
    bool open_for_peer(const uint8_t* peer, std::size_t peer_size,
                       const Telemetry::ShareSession* session);
    bool matches_peer(const uint8_t* peer, std::size_t peer_size) const;
    void apply_result(Telemetry::LocationConsentResult result,
                      const Telemetry::ShareSession* session);
    void set_back_callback(BackCallback callback) { back_callback_ = callback; }
    void set_start_callback(StartCallback callback) { start_callback_ = callback; }
    void set_stop_callback(StopCallback callback) { stop_callback_ = callback; }
    void show();
    void hide();

private:
    LocationShareControlModel model_;
    lv_obj_t* screen_;
    lv_obj_t* back_button_;
    lv_obj_t* state_label_;
    lv_obj_t* error_label_;
    lv_obj_t* duration_dropdown_;
    lv_obj_t* cadence_dropdown_;
    lv_obj_t* precision_dropdown_;
    lv_obj_t* start_button_;
    lv_obj_t* stop_button_;
    lv_obj_t* confirmation_dialog_;
    BackCallback back_callback_;
    StartCallback start_callback_;
    StopCallback stop_callback_;

    void refresh();
    void show_confirmation();
    static void on_back(lv_event_t* event);
    static void on_start(lv_event_t* event);
    static void on_stop(lv_event_t* event);
    static void on_selection(lv_event_t* event);
    static void on_confirmation(lv_event_t* event);
};

} // namespace LXMF
} // namespace UI
#endif
#endif
