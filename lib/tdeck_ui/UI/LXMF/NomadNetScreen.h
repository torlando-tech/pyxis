#pragma once
#ifdef ARDUINO
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <lvgl.h>
#include "NomadNetCompactPage.h"
#include "NomadNetDocument.h"
#include "NomadNetLibrary.h"
#include "NomadNetVirtualViewport.h"

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
    bool set_page(const NomadNet::Document& document);
    bool jump_to_anchor(const std::string& name);
    void restore_logical_scroll(int32_t logical);
    int32_t logical_scroll() const { return _logical_scroll; }
    bool page_loaded() const { return _page_loaded; }
    void set_library(const NomadNet::Library& library);
    void set_page_saved(bool saved);
    void begin_navigation(const std::string& target);
    void show_start();
    bool handle_library_back();
    bool directory_visible() const { return _directory_visible.load(std::memory_order_acquire); }
    void show(); void hide();
private:
    // Only the visible region plus bounded overscan is retained. The parser
    // admits at most 1024 runs, so this also covers a pathological viewport
    // containing every styled run plus bounded dividers.
    static constexpr std::size_t MAX_WINDOW_FRAGMENTS = 1056;
    static constexpr int32_t MAX_PHYSICAL_SCROLL_EXTENT = 30000;
    struct LayoutFragment {
        uint16_t run_index = 0;
        uint16_t byte_offset = 0;
        uint16_t byte_length = 0;
        int16_t link_index = -1;
        int16_t x = 0;
        int16_t y = 0;
        int16_t width = 0;
        int16_t height = 0;
        uint32_t divider_codepoint = 0x2500;
        bool divider = false;
        bool large_font = false;
        uint8_t heading_style = 0;
        LayoutFragment() = default;
        LayoutFragment(uint16_t run, uint16_t offset, uint16_t length, int16_t link,
                       int16_t left, int16_t top, int16_t w, int16_t h,
                       bool is_divider, bool is_large = false)
            : run_index(run), byte_offset(offset), byte_length(length), link_index(link),
              x(left), y(top), width(w), height(h), divider(is_divider), large_font(is_large) {}
        void set_heading(uint8_t level, bool starts_band) {
            heading_style = static_cast<uint8_t>((level & 0x03u) | (starts_band ? 0x80u : 0u));
        }
        uint8_t heading_level() const { return static_cast<uint8_t>(heading_style & 0x03u); }
        bool heading_starts_band() const { return (heading_style & 0x80u) != 0; }
    };
    struct LayoutCheckpoint {
        uint16_t block_index = 0;
        int32_t y = 0;
        LayoutCheckpoint() = default;
        LayoutCheckpoint(uint16_t block, int32_t top) : block_index(block), y(top) {}
    };
    lv_obj_t* _screen=nullptr; lv_obj_t* _back_button=nullptr; lv_obj_t* _home_button=nullptr;
    lv_obj_t* _reload_button=nullptr; lv_obj_t* _save_button=nullptr; lv_obj_t* _address_row=nullptr; lv_obj_t* _address=nullptr;
    lv_obj_t* _go_button=nullptr; lv_obj_t* _address_summary=nullptr; lv_obj_t* _edit_button=nullptr;
    lv_obj_t* _status=nullptr; lv_obj_t* _content=nullptr;
    lv_obj_t* _directory=nullptr;
    NomadNet::CompactPage _page;
    NomadNet::ExternalVector<LayoutFragment> _page_layout;
    NomadNet::ExternalVector<LayoutFragment> _line_layout;
    NomadNet::ExternalVector<LayoutCheckpoint> _layout_checkpoints;
    NomadNet::ExternalVector<int32_t> _link_y;
    NomadNet::ExternalVector<int32_t> _link_bottom;
    int32_t _page_height = 0;
    int32_t _physical_extent = 0;
    int32_t _logical_scroll = 0;
    int32_t _layout_window_top = 0;
    int32_t _layout_window_bottom = 0;
    int16_t _selected_link = -1;
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
    void clear_directory();
    bool layout_page();
    bool layout_window(int32_t logical_scroll);
    bool layout_from(std::size_t start_block, int32_t start_y,
                     int32_t window_top, int32_t window_bottom,
                     bool build_index);
    bool append_line_fragment(const LayoutFragment& fragment);
    bool commit_line(int32_t line_y, int16_t line_height,
                     NomadNet::Alignment alignment, int16_t indent,
                     int16_t available, uint8_t heading_level,
                     int32_t window_top, int32_t window_bottom);
    int32_t logical_scroll_from_widget() const;
    void scroll_to_logical(int32_t logical, lv_anim_enable_t animation);
    void draw_page(lv_event_t* event);
    void select_link(int direction);
    void activate_selected_link();
    void detach_focusables(lv_group_t* group);
    void rebuild_focus();
    static void clicked(lv_event_t* event);
    static void page_event(lv_event_t* event);
};
}
#endif
