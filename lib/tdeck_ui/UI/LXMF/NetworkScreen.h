#pragma once
#ifdef ARDUINO
#include <functional>
#include <lvgl.h>
namespace UI::LXMF {
class NetworkScreen {
public:
    using Callback = std::function<void()>;
    NetworkScreen(); ~NetworkScreen();
    void set_back_callback(Callback cb) { _back = std::move(cb); }
    void set_home_callback(Callback cb) { _home = std::move(cb); }
    void set_status_callback(Callback cb) { _callbacks[0] = std::move(cb); }
    void set_radio_activity_callback(Callback cb) { _callbacks[1] = std::move(cb); }
    void set_propagation_nodes_callback(Callback cb) { _callbacks[2] = std::move(cb); }
    void show(); void hide();
private:
    lv_obj_t* _screen = nullptr; lv_obj_t* _back_button = nullptr; lv_obj_t* _home_button = nullptr;
    lv_obj_t* _buttons[3]{}; Callback _back, _home, _callbacks[3];
    static void clicked(lv_event_t* event);
};
}
#endif
