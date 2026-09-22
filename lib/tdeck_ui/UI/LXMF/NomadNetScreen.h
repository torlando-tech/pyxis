#pragma once
#ifdef ARDUINO
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <lvgl.h>
#include "NomadNetCompactPage.h"
#include "NomadNetDocument.h"
#include "NomadNetForm.h"
#include "NomadNetLibrary.h"
#include "NomadNetPartialController.h"
#include "NomadNetVirtualViewport.h"

namespace UI::LXMF {
class NomadNetScreen {
public:
    using Callback = std::function<void()>;
    using OpenCallback = std::function<bool(const std::string&)>;
    using LinkCallback = std::function<bool(const std::string&)>;
    using SubmitCallback = std::function<bool(uint16_t, uint32_t)>;
    using SaveCallback = std::function<bool(const std::string&)>;
    using IdentifyCallback = std::function<bool(const std::string&, bool)>;
    NomadNetScreen(); ~NomadNetScreen();
    void set_back_callback(Callback cb) { _back = std::move(cb); }
    void set_home_callback(Callback cb) { _home = std::move(cb); }
    void set_reload_callback(OpenCallback cb) { _reload = std::move(cb); }
    void set_open_callback(OpenCallback cb) { _open = std::move(cb); }
    // Manual image-load button (upstream "Load images" / ctrl-l): the owner
    // loop re-admits this page's skipped images when the policy is MANUAL.
    // Returns false when the action queue is busy (screen shows a hint).
    void set_load_images_callback(OpenCallback cb) { _load_images = std::move(cb); }
    // Progress bar + status for the in-flight /media transfers (owner loop
    // owns the values; this screen only renders). The bar shows OVERALL page
    // progress (completed images + current transfer fraction) so a sequential
    // multi-image load visibly advances even between transfers. The status
    // line is written directly (not via set_status) to avoid re-layout on
    // every tick. Hidden when no image is loading.
    struct ImageProgress {
        bool active = false;
        std::size_t total_images = 0;   // images admitted for this page
        std::size_t position = 0;       // active image index, 1-based (0 = none)
        uint16_t percent = 0;           // 0-100 of the ACTIVE transfer
        std::size_t completed = 0;      // images already LOADED (bar baseline)
        uint64_t received_bytes = 0;    // bytes of the active transfer so far
        uint64_t total_bytes = 0;       // advertised size of the active transfer
        bool has_next = false;          // another image follows the finished one
        std::string status_text;        // written to the status line; empty = leave as-is
    };
    // The struct is passed by value so the screen can clamp percent without
    // mutating the owner's copy.
    void set_image_progress(ImageProgress progress);
    // True when the "Load images" button should be shown: the current page
    // has images that the MANUAL policy has not loaded yet.
    void set_images_actionable(bool actionable);
    void set_link_callback(LinkCallback cb) { _link = std::move(cb); }
    void set_submit_callback(SubmitCallback cb) { _submit = std::move(cb); }
    void set_save_callback(SaveCallback cb) { _save = std::move(cb); }
    void set_identify_callback(IdentifyCallback cb) { _identify = std::move(cb); }
    void set_address(const std::string& address);
    bool set_local_address(const std::string& address);
    std::string address() const;
    void set_status(const char* status);
    void clear_status();
    void set_partial_activity(bool active);
    bool set_page(const NomadNet::Document& document);
    // Main-thread decode seam: stores decoded RGB565 pixels for one page
    // image (bounded slot LRU, PSRAM) and returns true when the stored
    // image should reflow layout (intrinsic size unknown until now). The
    // owner loop calls this after a successful /media fetch + decode; it
    // must run only while the page generation is unchanged.
    bool set_page_image(uint16_t image_index, const uint16_t* rgb565,
                        uint32_t width, uint32_t height);
    // Post-decode reflow: rebuilds the full layout + focus index while
    // preserving logical scroll and selection state. No-op when the page
    // holds no image blocks. Returns false on layout failure (the page
    // stays as-is; the placeholder remains visible).
    bool relayout_images();
    // Reference-faithful image display sizing (NomadNet e1e8ab8
    // ImageWidget._display_size), exposed for host testing: computes the
    // placement rect of one image block inside the content box.
    struct ImagePlacement {
        int16_t x = 0;
        int16_t width = 0;
        int16_t height = 0;
    };
    static ImagePlacement compute_image_placement(
        uint16_t x0, uint32_t content_width, uint32_t content_height,
        const NomadNet::CompactPage::ImageRecord& record,
        uint32_t native_w, uint32_t native_h);
    bool prepare_submission(uint16_t link_id, uint32_t generation,
                            std::string& target,
                            NomadNet::ExternalVector<uint8_t>& request_data,
                            NomadNet::FormEncodeResult& result) const;
    bool prepare_partial_request(const NomadNet::PartialRequest& request,
                                 NomadNet::PartialController& controller,
                                 NomadNet::FormEncodeResult& result) const;
    bool partial_request_matches(
        const NomadNet::PartialRequest& request,
        const NomadNet::PartialController& controller) const {
        return controller.matches(request, _page);
    }
    NomadNet::PartialReplaceResult apply_partial_fragment(
        const NomadNet::PartialRequest& request,
        const NomadNet::Document& fragment,
        const NomadNet::PartialController& controller);
    bool partial_id_matches(std::size_t partial_index,
                            const char* id, std::size_t id_size) const;
    bool jump_to_anchor(const std::string& name);
    void restore_logical_scroll(int32_t logical);
#if defined(PYXIS_TEST_HOOKS) || defined(PYXIS_NOMAD_LINK_DIAGNOSTIC)
    void test_scroll(int32_t logical) { restore_logical_scroll(logical); }
#endif
    int32_t logical_scroll() const { return _logical_scroll; }
    bool page_loaded() const { return _page_loaded; }
    // The compact page currently rendered (used by the owner loop for
    // URL-identity checks around decoded image slots).
    const NomadNet::CompactPage& page() const { return _page; }
    void set_library(const NomadNet::Library& library);
    void set_page_saved(bool saved);
    void set_identify_enabled(bool enabled);
    void begin_navigation(const std::string& target);
    void show_pending_navigation(const std::string& target);
    void show_start();
    bool handle_library_back();
    bool directory_visible() const { return _directory_visible.load(std::memory_order_acquire); }
    void show(); void hide();
private:
    struct TableLayoutObservation {
        NomadNet::TableLayoutTier tier = NomadNet::TableLayoutTier::FIT;
        int16_t x = 0;
        int32_t y = 0;
        int16_t width = 0;
        int32_t height = 0;
        uint8_t columns = 0;
        uint16_t cards = 0;
        bool valid = false;
        TableLayoutObservation() = default;
        TableLayoutObservation(NomadNet::TableLayoutTier tier_value,
                               int16_t x_value, int32_t y_value,
                               int16_t width_value, int32_t height_value,
                               uint8_t columns_value, uint16_t cards_value,
                               bool valid_value)
            : tier(tier_value), x(x_value), y(y_value), width(width_value),
              height(height_value), columns(columns_value), cards(cards_value),
              valid(valid_value) {}
    };
    // Only the visible region plus bounded overscan is retained. The parser
    // admits at most 1024 runs, so this also covers a pathological viewport
    // containing every styled run plus bounded dividers.
    static constexpr std::size_t MAX_WINDOW_FRAGMENTS = 1600;
    static constexpr int32_t MAX_PHYSICAL_SCROLL_EXTENT = 30000;
    struct LayoutFragment {
        uint16_t run_index = 0;
        uint16_t byte_offset = 0;
        uint16_t byte_length = 0;
        int16_t link_index = -1;
        int16_t field_index = -1;
        int16_t x = 0;
        int16_t y = 0;
        int16_t width = 0;
        int16_t height = 0;
        uint32_t divider_codepoint = 0x2500;
        bool divider = false;
        bool large_font = false;
        bool table_cell = false;
        bool table_header = false;
        uint8_t heading_style = 0;
        // Image fragments: image_index indexes the page image record; the
        // placeholder is drawn until decode_image() publishes pixels.
        int16_t image_index = -1;
        uint8_t image_align = 0; // Alignment
        uint16_t image_native_w = 0;
        uint16_t image_native_h = 0;
        bool image_decoded = false;
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
    struct FocusTarget {
        uint16_t index = 0;
        int32_t y = 0;
        int32_t bottom = 0;
        uint16_t order = UINT16_MAX;
        bool field = false;
        FocusTarget() = default;
        FocusTarget(uint16_t item_index, int32_t top, int32_t lower, bool is_field,
                    uint16_t source_order = UINT16_MAX)
            : index(item_index), y(top), bottom(lower), order(source_order), field(is_field) {}
    };
    lv_obj_t* _screen=nullptr; lv_obj_t* _back_button=nullptr; lv_obj_t* _home_button=nullptr;
 lv_obj_t* _reload_button=nullptr; lv_obj_t* _save_button=nullptr; lv_obj_t* _identify_button=nullptr; lv_obj_t* _address_row=nullptr; lv_obj_t* _address=nullptr;
 lv_obj_t* _load_images_button=nullptr; lv_obj_t* _image_progress_bar=nullptr;
 lv_obj_t* _go_button=nullptr; lv_obj_t* _address_summary=nullptr; lv_obj_t* _edit_button=nullptr;
    lv_obj_t* _status=nullptr; lv_obj_t* _content=nullptr; lv_obj_t* _field_editor=nullptr;
    lv_timer_t* _status_timer=nullptr;
    lv_obj_t* _directory=nullptr;
    NomadNet::CompactPage _page;
    // Decoded page-image pixels. Bounded slot LRU in PSRAM; one 640x640
    // RGB565 slot is 768 KiB, so four slots cap at 3 MiB. Caps are
    // provisional: they must be re-measured in the physical low-water pass
    // before release (skill requirement), because a live page + LVGL +
    // map cache share the same PSRAM.
    static constexpr uint16_t MAX_DECODED_IMAGE_SLOTS = 4;
    struct DecodedImageSlot {
        uint16_t* pixels = nullptr; // lv_mem_alloc pool (LVGL src classification)
        uint32_t pixel_count = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t lru_rank = 0;
        uint16_t tag = 0; // compact image index this slot holds
        uint32_t url_hash = 0; // FNV-1a of the record URL at store time;
                               // a slot is only valid while the page's record
                               // at `tag` still has this URL (a partial
                               // refresh may replace region image records in
                               // place, same index, new URL)
        bool valid = false;
    };
    static uint32_t image_url_hash(const char* data, std::size_t length);
    // Slot/record URL-identity check (see slot_matches_record in .cpp).
    static bool slot_matches_record(const NomadNet::CompactPage& page,
                                    uint16_t image_index, uint32_t slot_url_hash);
    DecodedImageSlot _image_slots[MAX_DECODED_IMAGE_SLOTS];
    uint16_t _image_slot_rank = 0;
    const uint16_t* decoded_image(uint16_t image_index, uint16_t& width,
                                  uint16_t& height);
    void invalidate_page_images();
    NomadNet::FormState _form_state;
    NomadNet::ExternalVector<LayoutFragment> _page_layout;
    NomadNet::ExternalVector<LayoutFragment> _line_layout;
    NomadNet::ExternalVector<LayoutCheckpoint> _layout_checkpoints;
    NomadNet::ExternalVector<int32_t> _link_y;
    NomadNet::ExternalVector<int32_t> _link_bottom;
    NomadNet::ExternalVector<int32_t> _field_y;
    NomadNet::ExternalVector<int32_t> _field_bottom;
    NomadNet::ExternalVector<FocusTarget> _focus_order;
    int32_t _page_height = 0;
    int32_t _physical_extent = 0;
    int32_t _logical_scroll = 0;
    int32_t _layout_window_top = 0;
    int32_t _layout_window_bottom = 0;
    bool _transaction_scroll_restore = false;
#ifdef PYXIS_NOMADNET_TEST_HOOKS
    int8_t _test_scroll_fail_countdown = -1;
#endif
#ifdef PYXIS_TEST_HOOKS
    // Serial diagnostic: prints loader-independent slot state (tag/w/h/valid)
    // and per-page-image block count so index mapping can be verified against
    // the T:IMG lines during a physical image-load test.
    void test_image_state_dump() const;
#endif
    TableLayoutObservation _table_layout;
    int16_t _selected_link = -1;
    int16_t _selected_field = -1;
    int16_t _selected_focus = -1;
    int16_t _editing_field = -1;
    uint32_t _form_generation = 0;
    std::vector<lv_obj_t*> _directory_focusables;
    std::vector<std::string> _directory_targets;
    NomadNet::Library _library;
    enum class View { START, HEARD, SAVED_NODES, SAVED_PAGES, RECENT, BROWSER };
    View _view = View::START;
    std::atomic<bool> _directory_visible{true};
    bool _visible = false;
    bool _editing = true;
    bool _page_loaded = false;
    bool _identify_enabled = false;
    Callback _back,_home; OpenCallback _reload,_open,_load_images; LinkCallback _link;
    SubmitCallback _submit; SaveCallback _save; IdentifyCallback _identify;
    void set_address_editing(bool editing);
    static void status_timer_cb(lv_timer_t* timer);
    void cancel_status_timer();
    void apply_browser_layout(bool show_status);
    void render_directory(View view);
    void show_browser(bool editing);
    void clear_document();
    void clear_directory();
    bool layout_page();
    bool layout_window(int32_t logical_scroll);
    bool layout_table(const NomadNet::CompactPage::BlockRecord& block,
                      int32_t& y, int32_t window_top, int32_t window_bottom);
    bool layout_table_fit(const NomadNet::CompactPage::TableRecord& table,
                          const int16_t* column_widths, int16_t table_fit_width,
                          int32_t& y, int32_t window_top, int32_t window_bottom);
    bool layout_table_reflow(const NomadNet::CompactPage::TableRecord& table,
                             int32_t& y, int32_t window_top, int32_t window_bottom);
    bool layout_table_cell(const NomadNet::CompactPage::TableCellRecord& cell,
                           int16_t left, int16_t available, int32_t top,
                           int32_t window_top, int32_t window_bottom,
                           bool emit, int32_t& height);
    bool layout_from(std::size_t start_block, int32_t start_y,
                     int32_t window_top, int32_t window_bottom,
                     bool build_index);
    bool append_line_fragment(const LayoutFragment& fragment);
    bool commit_line(int32_t line_y, int16_t line_height,
                     NomadNet::Alignment alignment, int16_t indent,
                     int16_t available, uint8_t heading_level,
                     int32_t window_top, int32_t window_bottom);
    int32_t logical_scroll_from_widget() const;
    bool scroll_to_logical(int32_t logical, lv_anim_enable_t animation);
    void draw_page(lv_event_t* event);
    void select_link(int direction);
    void activate_selected_link();
    void begin_field_edit(uint16_t field_id);
    void finish_field_edit(bool accept);
    void detach_focusables(lv_group_t* group);
    void rebuild_focus();
    static void clicked(lv_event_t* event);
    static void page_event(lv_event_t* event);
    static void field_editor_event(lv_event_t* event);
};
}
#endif
