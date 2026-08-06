#pragma once
#ifdef ARDUINO
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <lvgl.h>
#include "NomadNetDocument.h"
#include "NomadNetLibrary.h"

namespace UI::LXMF {
class NomadNetScreen {
public:
    using Callback = std::function<void()>;
    using OpenCallback = std::function<bool(const std::string&)>;
    using LinkCallback = std::function<bool(const std::string&)>;
    using SaveCallback = std::function<bool(const std::string&)>;
    NomadNetScreen(); ~NomadNetScreen();
    void set_back_callback(Callback cb) { _back = std::move(cb); }
    void set_home_callback(Callback cb) { _home = std::move(cb); }
    void set_reload_callback(OpenCallback cb) { _reload = std::move(cb); }
    void set_open_callback(OpenCallback cb) { _open = std::move(cb); }
    void set_link_callback(LinkCallback cb) { _link = std::move(cb); }
    void set_save_callback(SaveCallback cb) { _save = std::move(cb); }
    void set_address(const std::string& address);
    std::string address() const;
    void set_status(const char* status);
    void set_page(const NomadNet::Document& document);
    void set_library(const NomadNet::Library& library);
    void set_page_saved(bool saved);
    void begin_navigation(const std::string& target);
    void show_start();
    bool handle_library_back();
    bool directory_visible() const { return _directory_visible.load(std::memory_order_acquire); }
    void show(); void hide();
private:
    static constexpr std::size_t MAX_UI_OBJECTS = 96;
    static constexpr std::size_t MAX_UI_SPANS = 256;
    lv_obj_t* _screen=nullptr; lv_obj_t* _back_button=nullptr; lv_obj_t* _home_button=nullptr;
    lv_obj_t* _reload_button=nullptr; lv_obj_t* _save_button=nullptr; lv_obj_t* _address_row=nullptr; lv_obj_t* _address=nullptr;
    lv_obj_t* _go_button=nullptr; lv_obj_t* _address_summary=nullptr; lv_obj_t* _edit_button=nullptr;
    lv_obj_t* _status=nullptr; lv_obj_t* _content=nullptr;
    lv_obj_t* _directory=nullptr;
    std::vector<lv_obj_t*> _focusables;
    std::vector<std::string> _link_targets;
    std::vector<lv_obj_t*> _directory_focusables;
    std::vector<std::string> _directory_targets;
    NomadNet::Library _library;
    enum class View { START, HEARD, SAVED_NODES, SAVED_PAGES, RECENT, BROWSER };
    View _view = View::START;
    std::atomic<bool> _directory_visible{true};
    bool _visible = false;
    bool _editing = true;
    bool _page_loaded = false;
    Callback _back,_home; OpenCallback _reload,_open; LinkCallback _link; SaveCallback _save;
    void set_address_editing(bool editing);
    void apply_browser_layout(bool show_status);
    void render_directory(View view);
    void show_browser(bool editing);
    void clear_document();
    void detach_focusables(lv_group_t* group);
    void rebuild_focus();
    static void clicked(lv_event_t* event);
};
}
#endif
