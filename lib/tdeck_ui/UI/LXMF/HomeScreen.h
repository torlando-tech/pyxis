#pragma once
#ifdef ARDUINO
#include <cstddef>
#include <functional>
#include <lvgl.h>

namespace UI::LXMF {
class HomeScreen {
public:
    using Callback = std::function<void()>;
    enum : std::size_t { APP_COUNT = 5 };
    HomeScreen();
    ~HomeScreen();
    void set_messages_callback(Callback cb) { _messages = std::move(cb); }
    void set_nomadnet_callback(Callback cb) { _nomadnet = std::move(cb); }
    void set_network_callback(Callback cb) { _network = std::move(cb); }
    void set_settings_callback(Callback cb) { _settings = std::move(cb); }
    void set_map_callback(Callback cb) { _map = std::move(cb); }
    void show();
    void hide();
private:
    lv_obj_t* _screen = nullptr;
    lv_obj_t* _buttons[APP_COUNT]{};
    Callback _messages, _nomadnet, _network, _settings, _map;
    static void clicked(lv_event_t* event);
};
}
#endif
