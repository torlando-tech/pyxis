#pragma once
#ifdef ARDUINO
#include <functional>
#include <string>
#include <vector>
#include <lvgl.h>
#include "NomadNetDocument.h"

namespace UI::LXMF {
class NomadNetScreen {
public:
    using Callback = std::function<void()>;
    using OpenCallback = std::function<void(const std::string&)>;
    using LinkCallback = std::function<void(const std::string&)>;
    NomadNetScreen(); ~NomadNetScreen();
    void set_back_callback(Callback cb) { _back = std::move(cb); }
    void set_home_callback(Callback cb) { _home = std::move(cb); }
    void set_reload_callback(Callback cb) { _reload = std::move(cb); }
    void set_open_callback(OpenCallback cb) { _open = std::move(cb); }
    void set_link_callback(LinkCallback cb) { _link = std::move(cb); }
    void set_address(const std::string& address);
    std::string address() const;
    void set_status(const char* status);
    void set_page(const NomadNet::Document& document);
    void show(); void hide();
private:
    static constexpr std::size_t MAX_UI_OBJECTS = 96;
    static constexpr std::size_t MAX_UI_SPANS = 256;
    lv_obj_t* _screen=nullptr; lv_obj_t* _back_button=nullptr; lv_obj_t* _home_button=nullptr;
    lv_obj_t* _reload_button=nullptr; lv_obj_t* _address_row=nullptr; lv_obj_t* _address=nullptr;
    lv_obj_t* _go_button=nullptr; lv_obj_t* _address_summary=nullptr; lv_obj_t* _edit_button=nullptr;
    lv_obj_t* _status=nullptr; lv_obj_t* _content=nullptr;
    std::vector<lv_obj_t*> _focusables;
    std::vector<std::string> _link_targets;
    bool _visible = false;
    bool _editing = true;
    bool _page_loaded = false;
    Callback _back,_home,_reload; OpenCallback _open; LinkCallback _link;
    void set_address_editing(bool editing);
    void apply_browser_layout(bool show_status);
    static void clicked(lv_event_t* event);
};
}
#endif
