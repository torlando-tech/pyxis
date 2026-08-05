#pragma once
#ifdef ARDUINO
#include <functional>
#include <lvgl.h>

namespace UI::LXMF {
class HomeScreen {
public:
    using Callback = std::function<void()>;
    HomeScreen();
    ~HomeScreen();
    void set_messages_callback(Callback cb) { _messages = std::move(cb); }
    void set_nomadnet_callback(Callback cb) { _nomadnet = std::move(cb); }
    void set_network_callback(Callback cb) { _network = std::move(cb); }
    void set_settings_callback(Callback cb) { _settings = std::move(cb); }
    void show();
    void hide();
private:
    lv_obj_t* _screen = nullptr;
    lv_obj_t* _buttons[4]{};
    Callback _messages, _nomadnet, _network, _settings;
    static void clicked(lv_event_t* event);
};
}
#endif
