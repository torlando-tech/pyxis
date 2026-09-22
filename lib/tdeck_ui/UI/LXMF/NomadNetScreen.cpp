#include "NomadNetScreen.h"
#ifdef ARDUINO
#include "Theme.h"
#include "NomadNetColors.h"
#include "NomadNetDisplay.h"
#include "NomadNetFocus.h"
#include "NomadNetGlyphs.h"
#include "../LVGL/LVGLInit.h"
#include "../TextAreaHelper.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <new>
#if defined(PYXIS_NOMAD_MEMORY_DIAGNOSTIC)
#include <esp_heap_caps.h>
#endif

namespace UI::LXMF {
namespace {
#if defined(PYXIS_NOMAD_MEMORY_DIAGNOSTIC)
void nomad_screen_heap_checkpoint(const char* phase) {
    Serial.printf(
        "T:NOMAD_HEAP phase=%s free=%u largest=%u minimum=%u psram=%u\n",
        phase,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
#else
void nomad_screen_heap_checkpoint(const char*) {}
#endif

const lv_font_t* navigation_font_12() {
    static const lv_font_t font=[] {
        lv_font_t font=lv_font_montserrat_12;
        font.fallback=&nomadnet_font_12;
        return font;
    }();
    return &font;
}

const lv_font_t* page_run_font(const NomadNet::CompactPage::RunRecord& run, bool large) {
    const bool bold = run.style & NomadNet::CompactPage::BOLD;
    const bool italic = run.style & NomadNet::CompactPage::ITALIC;
    if (large) {
        if (bold && italic) return &nomadnet_font_16_bold_italic;
        if (bold) return &nomadnet_font_16_bold;
        return italic ? &nomadnet_font_16_italic : &nomadnet_font_16;
    }
    if (bold && italic) return &nomadnet_font_12_bold_italic;
    if (bold) return &nomadnet_font_12_bold;
    return italic ? &nomadnet_font_12_italic : &nomadnet_font_12;
}

std::size_t safe_utf8_prefix(const char* value,std::size_t size,std::size_t limit){
    if(!value)return 0;
    if(size<=limit)return size;
    std::size_t retained=limit;
    while(retained>0&&(static_cast<unsigned char>(value[retained])&0xc0)==0x80)--retained;
    return retained;
}
}

// ── Page-image display sizing ──────────────────────────────────────────────
// Reference-faithful image sizing (NomadNet e1e8ab8 ImageWidget._display_size).
// The reference works in terminal columns/rows with a cell aspect ratio; the
// device reinterprets the numeric specs as bounded pixel budgets inside the
// 304 px content box and keeps the same four tiers:
//   both w and h -> stretch: target box, uniformly scaled down only when it
//                        overflows the available box
//   w only       -> width given, height derived preserving aspect
//   h only       -> height given, width derived preserving aspect
//   neither      -> full content width, height derived preserving aspect
// Without a decoded intrinsic size (placeholder state) the block reserves
// one body-font row, matching the reference's (1, 1) failure size.
NomadNetScreen::ImagePlacement NomadNetScreen::compute_image_placement(
        uint16_t x0, uint32_t content_width, uint32_t content_height,
        const NomadNet::CompactPage::ImageRecord& record,
        uint32_t native_w, uint32_t native_h) {
    ImagePlacement placement;
    if (native_w == 0 || native_h == 0) {
        placement.x = static_cast<int16_t>(x0);
        placement.width = static_cast<int16_t>(content_width);
        placement.height = 16; // one body-font row, drawn as placeholder text
        return placement;
    }
    const uint32_t iw = native_w;
    const uint32_t ih = native_h;
    const uint32_t maxcol = content_width;
    const uint32_t maxrow = content_height;
    // Device interpretation of the authored specs:
    //   NONE    -> unspecified (reference None)
    //   NATIVE  -> intrinsic size (reference "n")
    //   PIXELS  -> absolute pixel budget (reference int)
    //   PERCENT -> fraction of the content box (reference "NN%")
    const bool w_spec = record.width_kind != NomadNet::ImageDimension::NONE;
    const bool h_spec = record.height_kind != NomadNet::ImageDimension::NONE;
    auto raw_width = [&]() -> uint32_t {
        switch (record.width_kind) {
            case NomadNet::ImageDimension::NATIVE: return iw;
            case NomadNet::ImageDimension::PIXELS: return record.width_value;
            case NomadNet::ImageDimension::PERCENT:
                return (maxcol * std::min<uint32_t>(record.width_value, 100u)) / 100u;
            default: return maxcol;
        }
    };
    auto raw_height = [&]() -> uint32_t {
        switch (record.height_kind) {
            case NomadNet::ImageDimension::NATIVE: return ih;
            case NomadNet::ImageDimension::PIXELS: return record.height_value;
            case NomadNet::ImageDimension::PERCENT:
                return (maxrow * std::min<uint32_t>(record.height_value, 100u)) / 100u;
            default: return 0u;
        }
    };
    auto height_for_width = [&](uint32_t w) -> uint32_t {
        return (w * ih + iw / 2u) / (iw == 0 ? 1u : iw);
    };
    auto width_for_height = [&](uint32_t h) -> uint32_t {
        return (h * iw + ih / 2u) / (ih == 0 ? 1u : ih);
    };
    uint32_t cols = maxcol;
    uint32_t rows = 1;
    if (w_spec && h_spec) {
        const uint32_t target_w = std::max<uint32_t>(1u, raw_width());
        const uint32_t target_h = std::max<uint32_t>(1u, raw_height());
        if (target_w <= maxcol && target_h <= maxrow) {
            cols = target_w;
            rows = target_h;
        } else if (maxcol * target_h <= maxrow * target_w) {
            // Width binds the proportional downscale
            // (maxcol/target_w <= maxrow/target_h).
            cols = maxcol;
            rows = (maxcol * target_h + target_w / 2u) / (target_w == 0 ? 1u : target_w);
        } else {
            // Height binds.
            rows = maxrow;
            cols = (maxrow * target_w + target_h / 2u) / (target_h == 0 ? 1u : target_h);
        }
    } else if (w_spec) {
        cols = std::min<uint32_t>(maxcol, std::max<uint32_t>(1u, raw_width()));
        rows = std::max<uint32_t>(1u, height_for_width(cols));
        if (rows > maxrow) {
            rows = maxrow;
            cols = std::min<uint32_t>(maxcol, std::max<uint32_t>(1u, width_for_height(rows)));
        }
    } else if (h_spec) {
        rows = std::min<uint32_t>(maxrow, std::max<uint32_t>(1u, raw_height()));
        cols = std::min<uint32_t>(maxcol, std::max<uint32_t>(1u, width_for_height(rows)));
    } else {
        cols = maxcol;
        rows = std::max<uint32_t>(1u, height_for_width(cols));
        if (rows > maxrow) {
            rows = maxrow;
            cols = std::min<uint32_t>(maxcol, std::max<uint32_t>(1u, width_for_height(rows)));
        }
    }
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;
    placement.width = static_cast<int16_t>(cols);
    placement.height = static_cast<int16_t>(rows);
    int16_t left = static_cast<int16_t>(x0);
    if (record.align == NomadNet::Alignment::CENTER)
        left = static_cast<int16_t>(x0 + (content_width - cols) / 2u);
    else if (record.align == NomadNet::Alignment::RIGHT)
        left = static_cast<int16_t>(x0 + (content_width - cols));
    placement.x = left;
    return placement;
}

NomadNetScreen::NomadNetScreen() {
    _screen=lv_obj_create(lv_scr_act()); lv_obj_set_size(_screen,320,240);
    lv_obj_set_style_bg_color(_screen,Theme::surface(),0); lv_obj_set_style_border_width(_screen,0,0); lv_obj_set_style_pad_all(_screen,0,0);
    lv_obj_t* header=lv_obj_create(_screen); lv_obj_set_size(header,LV_PCT(100),34); lv_obj_align(header,LV_ALIGN_TOP_MID,0,0);
    lv_obj_set_style_bg_color(header,Theme::surfaceHeader(),0); lv_obj_set_style_border_width(header,0,0); lv_obj_set_style_pad_all(header,2,0);
    _back_button=lv_btn_create(header); lv_obj_set_size(_back_button,36,28); lv_obj_align(_back_button,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_t* bl=lv_label_create(_back_button);lv_label_set_text(bl,LV_SYMBOL_LEFT);lv_obj_center(bl);
    _home_button=lv_btn_create(header);lv_obj_set_size(_home_button,36,28);lv_obj_align(_home_button,LV_ALIGN_LEFT_MID,40,0);
    lv_obj_t* hl=lv_label_create(_home_button);lv_label_set_text(hl,LV_SYMBOL_HOME);lv_obj_center(hl);
    lv_obj_t* title=lv_label_create(header);lv_label_set_text(title,"NomadNet");lv_obj_set_style_text_font(title,&lv_font_montserrat_14,0);lv_obj_center(title);
    _reload_button=lv_btn_create(header);lv_obj_set_size(_reload_button,36,28);lv_obj_align(_reload_button,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_t* rl=lv_label_create(_reload_button);lv_label_set_text(rl,LV_SYMBOL_REFRESH);lv_obj_center(rl);
    _save_button=lv_btn_create(header);lv_obj_set_size(_save_button,36,28);lv_obj_align(_save_button,LV_ALIGN_RIGHT_MID,-40,0);
    lv_obj_t* sl=lv_label_create(_save_button);lv_label_set_text(sl,LV_SYMBOL_SAVE);lv_obj_center(sl);
    _identify_button=lv_btn_create(header);lv_obj_set_size(_identify_button,36,28);lv_obj_align(_identify_button,LV_ALIGN_RIGHT_MID,-80,0);
    lv_obj_t* il=lv_label_create(_identify_button);lv_label_set_text(il,"ID");lv_obj_center(il);
    // "Load images" (manual policy; upstream ctrl-l / "Load images"). Hidden
    // unless the current page has images the MANUAL policy has not loaded.
    _load_images_button=lv_btn_create(header);lv_obj_set_size(_load_images_button,36,28);lv_obj_align(_load_images_button,LV_ALIGN_LEFT_MID,80,0);
    lv_obj_t* lil=lv_label_create(_load_images_button);lv_label_set_text(lil,LV_SYMBOL_IMAGE);lv_obj_center(lil);
    for(auto* button:{_back_button,_home_button,_reload_button,_save_button,_identify_button,_load_images_button}) {
        lv_obj_set_style_bg_color(button,Theme::surfaceContainer(),0);
        lv_obj_set_style_bg_color(button,Theme::primaryPressed(),LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(button,0,0);
        lv_obj_set_style_radius(button,8,0);
    }

    _address_row=lv_obj_create(_screen);lv_obj_set_size(_address_row,LV_PCT(100),38);lv_obj_align(_address_row,LV_ALIGN_TOP_MID,0,34);
    lv_obj_set_style_bg_color(_address_row,Theme::surface(),0);lv_obj_set_style_border_width(_address_row,0,0);lv_obj_set_style_pad_all(_address_row,3,0);
    _address=lv_textarea_create(_address_row);lv_obj_set_size(_address,270,30);lv_obj_align(_address,LV_ALIGN_LEFT_MID,0,0);
    lv_textarea_set_one_line(_address,true);lv_textarea_set_max_length(_address,511);lv_textarea_set_placeholder_text(_address,"destination:/page/path");
    lv_obj_set_style_text_font(_address,navigation_font_12(),0);lv_obj_set_style_bg_color(_address,Theme::surfaceInput(),0);TextAreaHelper::enable_paste(_address);
    _go_button=lv_btn_create(_address_row);lv_obj_set_size(_go_button,42,30);lv_obj_align(_go_button,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_set_style_bg_color(_go_button,Theme::primary(),0);lv_obj_set_style_radius(_go_button,8,0);
    lv_obj_t* gl=lv_label_create(_go_button);lv_label_set_text(gl,"Go");lv_obj_center(gl);

    _address_summary=lv_label_create(_address_row);lv_obj_set_size(_address_summary,266,24);lv_obj_align(_address_summary,LV_ALIGN_LEFT_MID,4,0);
    lv_label_set_long_mode(_address_summary,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(_address_summary,navigation_font_12(),0);
    lv_obj_set_style_text_color(_address_summary,Theme::textSecondary(),0);
    _edit_button=lv_btn_create(_address_row);lv_obj_set_size(_edit_button,36,26);lv_obj_align(_edit_button,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_set_style_bg_color(_edit_button,Theme::surfaceContainer(),0);lv_obj_set_style_bg_color(_edit_button,Theme::primaryPressed(),LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(_edit_button,0,0);lv_obj_set_style_radius(_edit_button,8,0);
    lv_obj_t* edit=lv_label_create(_edit_button);lv_label_set_text(edit,LV_SYMBOL_EDIT);lv_obj_center(edit);
    lv_obj_add_flag(_address_summary,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(_edit_button,LV_OBJ_FLAG_HIDDEN);

    _status=lv_label_create(_screen);lv_obj_set_size(_status,312,18);lv_obj_align(_status,LV_ALIGN_TOP_LEFT,4,72);
    lv_obj_set_style_text_font(_status,&lv_font_montserrat_12,0);lv_obj_set_style_text_color(_status,Theme::textTertiary(),0);
    // Image-transfer progress bar: a thin full-width strip just below the
    // header. Owner loop sets the value while a /media request is in flight;
    // it is hidden otherwise. Kept out of the header so it never steals
    // focus from the address/tools and never disturbs page layout.
    _image_progress_bar=lv_bar_create(_screen);lv_obj_set_size(_image_progress_bar,320,4);lv_obj_align(_image_progress_bar,LV_ALIGN_TOP_LEFT,0,34);
    lv_bar_set_range(_image_progress_bar,0,100);lv_bar_set_value(_image_progress_bar,0,LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(_image_progress_bar,LV_OPA_20,LV_PART_MAIN);
    lv_obj_set_style_bg_color(_image_progress_bar,Theme::border(),LV_PART_MAIN);
    lv_obj_set_style_bg_color(_image_progress_bar,Theme::primary(),LV_PART_INDICATOR);
    lv_obj_add_flag(_image_progress_bar,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_image_progress_bar,LV_OBJ_FLAG_CLICKABLE);
    _content=lv_obj_create(_screen);lv_obj_set_size(_content,320,150);lv_obj_align(_content,LV_ALIGN_BOTTOM_MID,0,0);
    lv_obj_set_style_bg_color(_content,Theme::surface(),0);lv_obj_set_style_border_width(_content,0,0);lv_obj_set_style_pad_all(_content,8,0);
    lv_obj_set_flex_flow(_content,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(_content,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
    lv_obj_add_flag(_content,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_content,LV_DIR_VER);lv_obj_set_scrollbar_mode(_content,LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(_content,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_content,page_event,LV_EVENT_ALL,this);

    _directory=lv_obj_create(_screen);lv_obj_set_size(_directory,320,206);lv_obj_align(_directory,LV_ALIGN_BOTTOM_MID,0,0);
    lv_obj_set_style_bg_color(_directory,Theme::surface(),0);lv_obj_set_style_border_width(_directory,0,0);lv_obj_set_style_pad_all(_directory,7,0);
    lv_obj_set_flex_flow(_directory,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(_directory,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(_directory,4,0);lv_obj_set_scroll_dir(_directory,LV_DIR_VER);lv_obj_set_scrollbar_mode(_directory,LV_SCROLLBAR_MODE_AUTO);
    for(auto* o:{_back_button,_home_button,_reload_button,_save_button,_identify_button,_load_images_button,_go_button,_edit_button})lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,this);
    lv_obj_add_event_cb(_address,clicked,LV_EVENT_READY,this);
    lv_obj_add_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_identify_button,LV_OBJ_FLAG_HIDDEN);
    set_status("Enter a NomadNet address");show_start();hide();
}
NomadNetScreen::~NomadNetScreen(){
    cancel_status_timer();
    finish_field_edit(false);
    if(_field_editor)lv_textarea_set_text(_field_editor,"");
    if(_screen)lv_obj_del(_screen);
}
void NomadNetScreen::set_address(const std::string& value){
    const auto summary=NomadNet::display_text(NomadNet::compact_address(value));
    lv_textarea_set_text(_address,value.c_str());
    lv_label_set_text(_address_summary,summary.c_str());
}
bool NomadNetScreen::set_local_address(const std::string& value){
    // A local navigation changes only the fragment. The compact destination
    // summary is therefore already correct. The patched textarea replacement
    // is transactional; verify publication without allocating before changing
    // scroll or committing the staged history transaction.
    lv_textarea_set_text(_address,value.c_str());
    return std::strcmp(lv_textarea_get_text(_address),value.c_str())==0;
}
std::string NomadNetScreen::address()const{return lv_textarea_get_text(_address);}
void NomadNetScreen::set_library(const NomadNet::Library& library){
    _library=library;
    if(_view!=View::BROWSER)render_directory(_view);
}
void NomadNetScreen::set_page_saved(bool saved){
    lv_obj_set_style_bg_color(_save_button,saved?Theme::primary():Theme::surfaceContainer(),0);
    if(_page_loaded)lv_obj_clear_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
}
void NomadNetScreen::set_identify_enabled(bool enabled){
    _identify_enabled=enabled;
    lv_obj_set_style_bg_color(_identify_button,enabled?Theme::primary():Theme::surfaceContainer(),0);
    if(_page_loaded){
        lv_obj_clear_flag(_identify_button,LV_OBJ_FLAG_HIDDEN);
        rebuild_focus();
    }
}
void NomadNetScreen::set_image_progress(ImageProgress progress){
    if(!progress.active){
        lv_obj_add_flag(_image_progress_bar,LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if(progress.percent>100)progress.percent=100;
    // Overall page fraction: each admitted image is an equal slice; the
    // active transfer contributes its own percent within its slice. A
    // finished-success adds the just-completed slice (the loader has not
    // yet counted it as loaded); a finished-failure fills nothing (the
    // next image's first tick resumes the advance).
    const double total=progress.total_images>0?static_cast<double>(progress.total_images):1.0;
    double overall=static_cast<double>(progress.completed)/total*100.0;
    if(progress.position>0&&progress.position<=progress.total_images){
        overall+=static_cast<double>(progress.percent)/100.0/total*100.0;
    }
    if(overall>100.0)overall=100.0;
    const bool last_finished=progress.percent==100&&!progress.has_next;
    if(!last_finished){
        lv_obj_clear_flag(_image_progress_bar,LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(_image_progress_bar,static_cast<int32_t>(overall+0.5),LV_ANIM_OFF);
    }else{
        // Last image done: the bar has served its purpose; the status line
        // now shows the summary until the next page/refresh.
        lv_obj_add_flag(_image_progress_bar,LV_OBJ_FLAG_HIDDEN);
    }
    if(!progress.status_text.empty()){
        // Direct label write (not set_status): progress ticks are frequent
        // and must not churn the browser layout or the status timers.
        lv_obj_clear_flag(_status,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(_status,progress.status_text.c_str());
    }
}
void NomadNetScreen::set_images_actionable(bool actionable){
    if(actionable&&_page_loaded&&!_page.images().empty()){
        lv_obj_clear_flag(_load_images_button,LV_OBJ_FLAG_HIDDEN);
    }else{
        lv_obj_add_flag(_load_images_button,LV_OBJ_FLAG_HIDDEN);
    }
}
void NomadNetScreen::clear_document(){
    nomad_screen_heap_checkpoint("clear-before");
    finish_field_edit(false);
    ++_form_generation;
    _form_state.clear();
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        if(lv_group_get_editing(group)&&lv_group_get_focused(group)==_content)lv_group_set_editing(group,false);
        lv_group_remove_obj(_content);
    }
    _page.clear();
    NomadNet::ExternalVector<LayoutFragment>().swap(_page_layout);
    NomadNet::ExternalVector<LayoutFragment>().swap(_line_layout);
    NomadNet::ExternalVector<LayoutCheckpoint>().swap(_layout_checkpoints);
    NomadNet::ExternalVector<int32_t>().swap(_link_y);
    NomadNet::ExternalVector<int32_t>().swap(_link_bottom);
    NomadNet::ExternalVector<int32_t>().swap(_field_y);
    NomadNet::ExternalVector<int32_t>().swap(_field_bottom);
    NomadNet::ExternalVector<FocusTarget>().swap(_focus_order);
    _page_height=0;
    _physical_extent=0;
    _logical_scroll=0;
    _layout_window_top=0;
    _layout_window_bottom=0;
    _table_layout=TableLayoutObservation{};
    _selected_link=-1;
    _selected_field=-1;
    _selected_focus=-1;
    _editing_field=-1;
    lv_obj_refresh_self_size(_content);
    lv_obj_invalidate(_content);
    _page_loaded=false;
    set_page_saved(false);
    lv_obj_add_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_identify_button,LV_OBJ_FLAG_HIDDEN);
    nomad_screen_heap_checkpoint("clear-after");
}
void NomadNetScreen::clear_directory(){
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group)detach_focusables(group);
    lv_obj_clean(_directory);
    std::vector<lv_obj_t*>().swap(_directory_focusables);
    std::vector<std::string>().swap(_directory_targets);
}
void NomadNetScreen::begin_navigation(const std::string& target){
    clear_document();
    clear_directory();
    set_address(target);
    show_browser(false);
    set_status("Opening NomadNet page...");
}
void NomadNetScreen::show_pending_navigation(const std::string& target){
    set_address(target);
    show_browser(false);
    set_status("Opening NomadNet page...");
}
void NomadNetScreen::show_start(){
    clear_document();
    render_directory(View::START);
}
bool NomadNetScreen::handle_library_back(){
    if(_view==View::START)return false;
    show_start();
    return true;
}
void NomadNetScreen::show_browser(bool editing){
    _view=View::BROWSER;
    _directory_visible.store(false,std::memory_order_release);
    lv_obj_add_flag(_directory,LV_OBJ_FLAG_HIDDEN);
    for(auto* object:{_address_row,_status,_content,_reload_button})lv_obj_clear_flag(object,LV_OBJ_FLAG_HIDDEN);
    if(_page_loaded){
        lv_obj_clear_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_identify_button,LV_OBJ_FLAG_HIDDEN);
    }else{
        lv_obj_add_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_identify_button,LV_OBJ_FLAG_HIDDEN);
    }
    // The manual image-load button re-appears via set_images_actionable() once
    // the owner loop has configured the page's image loader.
    lv_obj_add_flag(_load_images_button,LV_OBJ_FLAG_HIDDEN);
    set_address_editing(editing);
    rebuild_focus();
}
void NomadNetScreen::render_directory(View view){
    clear_directory();
    _view=view;
    _directory_visible.store(true,std::memory_order_release);
    lv_obj_clear_flag(_directory,LV_OBJ_FLAG_HIDDEN);
    for(auto* object:{_address_row,_status,_content,_reload_button,_save_button,_identify_button,_load_images_button})lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);

    // LVGL automatically enrols newly constructed group-capable widgets in the
    // default group and focuses the first member of an empty group. Keep the
    // replacement rows detached until rebuild_focus() can install the complete
    // membership and choose one final owner.
    auto* previous_default_group=lv_group_get_default();
    lv_group_set_default(nullptr);

    auto add_row=[&](const std::string& title,const std::string& detail,std::size_t code,const char* symbol=nullptr){
        lv_obj_t* button=lv_btn_create(_directory);lv_obj_set_size(button,306,35);lv_obj_set_flex_grow(button,0);
        lv_obj_set_style_bg_color(button,Theme::surfaceContainer(),0);lv_obj_set_style_bg_color(button,Theme::primaryPressed(),LV_STATE_FOCUSED);
        // The default theme adds a blue outline for keypad focus on top of this
        // purple focused background. Keep one stable visual focus treatment.
        lv_obj_set_style_outline_width(button,0,LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(button,0,0);lv_obj_set_style_radius(button,8,0);lv_obj_set_style_pad_all(button,4,0);
        lv_coord_t title_x=2;
        lv_coord_t title_width=286;
        if(symbol){
            lv_obj_t* icon=lv_label_create(button);lv_label_set_text(icon,symbol);
            lv_obj_set_style_text_font(icon,&lv_font_montserrat_12,0);lv_obj_align(icon,LV_ALIGN_TOP_LEFT,2,0);
            title_x=20;title_width=268;
        }
        const auto rendered_title=NomadNet::display_text(title);
        lv_obj_t* primary=lv_label_create(button);lv_label_set_text(primary,rendered_title.c_str());lv_label_set_long_mode(primary,LV_LABEL_LONG_DOT);
        lv_obj_set_width(primary,title_width);lv_obj_set_style_text_font(primary,navigation_font_12(),0);lv_obj_align(primary,LV_ALIGN_TOP_LEFT,title_x,0);
        if(!detail.empty()){
            const auto rendered_detail=NomadNet::display_text(detail);
            lv_obj_t* secondary=lv_label_create(button);lv_label_set_text(secondary,rendered_detail.c_str());lv_label_set_long_mode(secondary,LV_LABEL_LONG_DOT);
            lv_obj_set_width(secondary,286);lv_obj_set_style_text_font(secondary,navigation_font_12(),0);lv_obj_set_style_text_color(secondary,Theme::textTertiary(),0);
            lv_obj_align(secondary,LV_ALIGN_BOTTOM_LEFT,2,0);
        }
        lv_obj_set_user_data(button,reinterpret_cast<void*>(code));lv_obj_add_event_cb(button,clicked,LV_EVENT_CLICKED,this);
        _directory_focusables.push_back(button);
    };

    if(view==View::START){
        add_row("Heard Nodes","Recently announced NomadNet nodes",1001,LV_SYMBOL_DIRECTORY);
        add_row("Saved Nodes","Bookmarked destinations",1002,LV_SYMBOL_DIRECTORY);
        add_row("Saved Pages","Bookmarked destination and path",1003,LV_SYMBOL_SAVE);
        add_row("Recent Pages","Bounded browsing history",1004,LV_SYMBOL_LIST);
        add_row("Enter Address","Advanced destination/path entry",1005,LV_SYMBOL_EDIT);
    }else if(view==View::HEARD||view==View::SAVED_NODES){
        for(const auto& node:_library.nodes()){
            if(view==View::SAVED_NODES&&!node.saved)continue;
            const std::string target=node.destination_hex+":/page/index.mu";
            _directory_targets.push_back(target);
            char detail[48];
            std::snprintf(detail,sizeof(detail),"%s / %s",node.hops==0?"Direct":(std::to_string(node.hops)+" hops").c_str(),node.saved?"Saved":"Heard");
            add_row(node.name.empty()?NomadNet::compact_address(target,28):node.name,detail,2000+_directory_targets.size());
        }
    }else{
        for(const auto& page:_library.pages()){
            if(view==View::SAVED_PAGES&&!page.saved)continue;
            _directory_targets.push_back(page.url);
            const auto colon=page.url.find(':');
            const std::string path=colon==std::string::npos?page.url:page.url.substr(colon+1);
            add_row(page.title.empty()?NomadNet::compact_address(page.url,28):page.title,path,2000+_directory_targets.size());
        }
    }
    if(_directory_focusables.empty()){
        lv_obj_t* empty=lv_label_create(_directory);
        lv_label_set_text(empty,view==View::HEARD?"No NomadNet nodes heard yet":view==View::SAVED_NODES?"No saved nodes":view==View::SAVED_PAGES?"No saved pages":"No recent pages");
        lv_obj_set_style_text_color(empty,Theme::textTertiary(),0);lv_obj_set_style_text_font(empty,navigation_font_12(),0);
    }
    lv_group_set_default(previous_default_group);
    rebuild_focus();
}
void NomadNetScreen::detach_focusables(lv_group_t* group){
    if(!group)return;
    auto* focused=lv_group_get_focused(group);
    auto owned_focusable=[&](lv_obj_t* candidate){
        if(!candidate)return false;
        for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_identify_button,_address,_go_button,_edit_button})
            if(candidate==object)return true;
        if(candidate==_content)return true;
        for(auto* object:_directory_focusables)if(candidate==object)return true;
        return false;
    };
    auto detach_non_owner=[&](lv_obj_t* object){
        if(object&&object!=focused)lv_group_remove_obj(object);
    };
    for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_identify_button,_address,_go_button,_edit_button})detach_non_owner(object);
    detach_non_owner(_content);
    for(auto* object:_directory_focusables)detach_non_owner(object);
    if(focused&&owned_focusable(focused))lv_group_remove_obj(focused);
}
void NomadNetScreen::rebuild_focus(){
    if(!_visible)return;
    auto* group=LVGL::LVGLInit::get_default_group();if(!group)return;
    detach_focusables(group);
    // Adding the first member to an empty LVGL group normally focuses it. Freeze
    // while restoring navigation order, then assign the intended owner once.
    lv_group_focus_freeze(group,true);
    lv_group_add_obj(group,_back_button);lv_group_add_obj(group,_home_button);
    if(_view!=View::BROWSER){
        for(auto* object:_directory_focusables)lv_group_add_obj(group,object);
        auto* final_focus=!_directory_focusables.empty()?_directory_focusables.front():_back_button;
        lv_group_focus_freeze(group,false);
        lv_group_focus_obj(final_focus);
        return;
    }
    lv_group_add_obj(group,_reload_button);
    if(!lv_obj_has_flag(_save_button,LV_OBJ_FLAG_HIDDEN))lv_group_add_obj(group,_save_button);
    if(!lv_obj_has_flag(_identify_button,LV_OBJ_FLAG_HIDDEN))lv_group_add_obj(group,_identify_button);
    if(!lv_obj_has_flag(_load_images_button,LV_OBJ_FLAG_HIDDEN))lv_group_add_obj(group,_load_images_button);
    if(_editing){lv_group_add_obj(group,_address);lv_group_add_obj(group,_go_button);}
    else lv_group_add_obj(group,_edit_button);
    if(!_page.empty())lv_group_add_obj(group,_content);
    // Keep a newly loaded document at its beginning. Focusing the first link
    // makes LVGL auto-scroll that link into view, which can hide the heading.
    lv_group_focus_freeze(group,false);
    lv_group_focus_obj(_editing?_address:_edit_button);
}
void NomadNetScreen::apply_browser_layout(bool show_status){
    if(_editing){
        lv_obj_set_height(_address_row,38);lv_obj_align(_address_row,LV_ALIGN_TOP_MID,0,34);
        lv_obj_align(_status,LV_ALIGN_TOP_LEFT,4,72);
        lv_obj_set_height(_content,show_status?150:168);
    }else{
        lv_obj_set_height(_address_row,30);lv_obj_align(_address_row,LV_ALIGN_TOP_MID,0,34);
        lv_obj_align(_status,LV_ALIGN_TOP_LEFT,4,64);
        lv_obj_set_height(_content,show_status?158:176);
    }
    if(show_status)lv_obj_clear_flag(_status,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(_status,LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_content,LV_ALIGN_BOTTOM_MID,0,0);
}
void NomadNetScreen::set_address_editing(bool editing){
    auto* group=LVGL::LVGLInit::get_default_group();
    if(_visible&&group){lv_group_remove_obj(_address);lv_group_remove_obj(_go_button);lv_group_remove_obj(_edit_button);}
    _editing=editing;
    if(editing){
        lv_obj_clear_flag(_address,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(_go_button,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_address_summary,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(_edit_button,LV_OBJ_FLAG_HIDDEN);
    }else{
        lv_obj_add_flag(_address,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(_go_button,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_address_summary,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(_edit_button,LV_OBJ_FLAG_HIDDEN);
    }
    const bool show_status=!lv_obj_has_flag(_status,LV_OBJ_FLAG_HIDDEN);
    apply_browser_layout(show_status);
    if(_visible&&group){
        if(editing){lv_group_add_obj(group,_address);lv_group_add_obj(group,_go_button);lv_group_focus_obj(_address);}
        else{lv_group_add_obj(group,_edit_button);lv_group_focus_obj(_edit_button);}
    }
}
void NomadNetScreen::cancel_status_timer(){
    if(!_status_timer)return;
    lv_timer_del(_status_timer);
    _status_timer=nullptr;
}
void NomadNetScreen::status_timer_cb(lv_timer_t* timer){
    auto* screen=static_cast<NomadNetScreen*>(timer->user_data);
    if(!screen||screen->_status_timer!=timer)return;
    screen->_status_timer=nullptr;
    screen->apply_browser_layout(false);
}
void NomadNetScreen::clear_status(){
    cancel_status_timer();
    if(_status)lv_label_set_text(_status,"");
    apply_browser_layout(false);
}
void NomadNetScreen::set_partial_activity(bool active){
    if(!_reload_button)return;
    lv_obj_set_style_bg_color(_reload_button,
        active?Theme::primary():Theme::surfaceContainer(),LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_reload_button,
        active?Theme::primary():Theme::primaryPressed(),LV_STATE_FOCUSED);
    if(lv_obj_t* icon=lv_obj_get_child(_reload_button,0))
        lv_obj_set_style_text_color(icon,active?Theme::warning():Theme::textPrimary(),0);
    lv_obj_invalidate(_reload_button);
}
void NomadNetScreen::set_status(const char* value){
    cancel_status_timer();
    const char* status=value?value:"";
    lv_label_set_text(_status,status);
    const bool loaded_ack=_page_loaded&&std::strncmp(status,"Page loaded",11)==0;
    const bool cached_ack=_page_loaded&&std::strncmp(status,"Cached page",11)==0;
    apply_browser_layout(!loaded_ack);
    if(cached_ack){
        _status_timer=lv_timer_create(status_timer_cb,1500,this);
        if(_status_timer)lv_timer_set_repeat_count(_status_timer,1);
        else apply_browser_layout(false);
    }
}
bool NomadNetScreen::set_page(const NomadNet::Document& document) {
    // Prepare every heap-backed model before touching the published page. The
    // temporary owns and releases partial capacities on every failure.
    NomadNet::CompactPage candidate_page;
    NomadNet::FormState candidate_form;
    try {
        if(!candidate_page.assign(document)||!candidate_form.assign(candidate_page)){
            set_status("Page is too large for available memory");
            return false;
        }
        if(candidate_page.truncated()){
            const std::string notice=NomadNet::truncation_notice(document);
            if(!candidate_page.append_notice(notice)){
                set_status("Page truncation notice could not be retained");
                return false;
            }
        }else if(candidate_page.unsupported()&&
                 !candidate_page.append_notice("[Unsupported Micron content]")){
            set_status("Unsupported-content notice could not be retained");
            return false;
        }
    }catch(const std::bad_alloc&){
        set_status("Page is too large for available memory");
        return false;
    }

    auto old_page=std::move(_page);
    auto old_form=std::move(_form_state);
    auto old_page_layout=std::move(_page_layout);
    auto old_line_layout=std::move(_line_layout);
    auto old_checkpoints=std::move(_layout_checkpoints);
    auto old_link_y=std::move(_link_y);
    auto old_link_bottom=std::move(_link_bottom);
    auto old_field_y=std::move(_field_y);
    auto old_field_bottom=std::move(_field_bottom);
    auto old_focus=std::move(_focus_order);
    const int32_t old_page_height=_page_height,old_physical_extent=_physical_extent;
    const int32_t old_logical_scroll=_logical_scroll,old_window_top=_layout_window_top;
    const int32_t old_window_bottom=_layout_window_bottom;
    const TableLayoutObservation old_table_layout=_table_layout;
    const int16_t old_selected_link=_selected_link,old_selected_field=_selected_field;
    const int16_t old_selected_focus=_selected_focus;
    const bool old_page_loaded=_page_loaded;

    _page=std::move(candidate_page);
    _form_state=std::move(candidate_form);
    bool laid_out=false;
    try{laid_out=layout_page();}catch(const std::bad_alloc&){laid_out=false;}
    if(!laid_out){
        _page=std::move(old_page);_form_state=std::move(old_form);
        _page_layout=std::move(old_page_layout);_line_layout=std::move(old_line_layout);
        _layout_checkpoints=std::move(old_checkpoints);_link_y=std::move(old_link_y);
        _link_bottom=std::move(old_link_bottom);_field_y=std::move(old_field_y);
        _field_bottom=std::move(old_field_bottom);_focus_order=std::move(old_focus);
        _page_height=old_page_height;_physical_extent=old_physical_extent;
        _logical_scroll=old_logical_scroll;_layout_window_top=old_window_top;
        _layout_window_bottom=old_window_bottom;_table_layout=old_table_layout;
        _selected_link=old_selected_link;
        _selected_field=old_selected_field;_selected_focus=old_selected_focus;
        _page_loaded=old_page_loaded;
        set_status("Page viewport could not be retained");
        return false;
    }

    auto* group=LVGL::LVGLInit::get_default_group();
    if(group)lv_group_remove_obj(_content);
    finish_field_edit(false);
    ++_form_generation;
    show_browser(false);
    lv_obj_set_style_bg_color(_content,_page.has_background()
        ?lv_color_hex(_page.background()):Theme::surface(),0);
    _page_loaded=true;
    invalidate_page_images();
    lv_obj_scroll_to_y(_content,0,LV_ANIM_OFF);
    lv_obj_refresh_self_size(_content);
    lv_obj_invalidate(_content);
    return true;
}

// ── Page-image pixel store ────────────────────────────────────────────────
// A decode publishes one RGB565 buffer into a bounded LRU slot. The slot
// owns the pixels until the next page invalidates them, so the draw path
// can blit them without re-decoding. Eviction is rank-based; the least
// recently used valid slot is reused when all are occupied.
void NomadNetScreen::invalidate_page_images() {
    for (auto& slot : _image_slots) {
        if (slot.pixels) lv_mem_free(slot.pixels);
        slot.pixels = nullptr;
        slot.pixel_count = 0;
        slot.width = 0;
        slot.height = 0;
        slot.valid = false;
    }
    _image_slot_rank = 0;
}

uint32_t NomadNetScreen::image_url_hash(const char* data, std::size_t length) {
    // FNV-1a 32-bit; empty (blanked) records hash to the offset basis so
    // they never match a stored (non-empty) URL.
    uint32_t value = 2166136261u;
    for (std::size_t i = 0; i < length; ++i) {
        value = static_cast<uint32_t>(
            (value ^ static_cast<unsigned char>(data[i])) * 16777619u);
    }
    return value;
}

// True when the page's image record at `index` (if any) still matches the
// URL the slot was stored for. A partial refresh replaces region image
// records IN PLACE (same compact index, possibly a new URL); without this
// check a decoded slot would keep showing the old image's pixels under the
// new URL, and an index-shifted list would alias one image's pixels onto
// another. Blank (replaced-away) records have an empty URL and mismatch
// any stored hash.
bool NomadNetScreen::slot_matches_record(const NomadNet::CompactPage& page,
                                         uint16_t image_index,
                                         uint32_t slot_url_hash) {
    if (image_index >= page.images().size()) return false;
    const auto url = page.image_url(page.images()[image_index]);
    return image_url_hash(url.data(), url.size()) == slot_url_hash;
}

const uint16_t* NomadNetScreen::decoded_image(uint16_t image_index,
                                              uint16_t& width, uint16_t& height) {
    width = 0;
    height = 0;
    if (image_index >= _page.images().size()) return nullptr;
    // Slots are tagged with the compact image index they currently hold.
    for (uint16_t s = 0; s < MAX_DECODED_IMAGE_SLOTS; ++s) {
        auto& slot = _image_slots[s];
        if (slot.valid && slot.tag == image_index &&
                slot_matches_record(_page, image_index, slot.url_hash)) {
            width = slot.width;
            height = slot.height;
            return slot.pixels;
        }
    }
    return nullptr;
}

bool NomadNetScreen::set_page_image(uint16_t image_index, const uint16_t* rgb565,
                                    uint32_t width, uint32_t height) {
    if (!rgb565 || width == 0 || height == 0) return false;
    if (width > 640 || height > 640) return false; // decoder cap
    if (image_index >= _page.images().size()) return false;
    // The slot must be tagged with the record URL at store time so the draw
    // path can reject it if a partial refresh later replaces the record in
    // place (same index, new or blank URL).
    const auto url = _page.image_url(
        _page.images()[static_cast<std::size_t>(image_index)]);
    const uint64_t pixel_count = static_cast<uint64_t>(width) * height;
    if (pixel_count > 640u * 640u) return false;

    // Prefer a slot already holding this image; otherwise evict LRU.
    int16_t target = -1;
    int16_t lru = -1;
    for (uint16_t s = 0; s < MAX_DECODED_IMAGE_SLOTS; ++s) {
        auto& slot = _image_slots[s];
        if (slot.valid && slot.tag == image_index &&
                slot_matches_record(_page, image_index, slot.url_hash)) {
            target = s;
            break;
        }
        if (!slot.valid || (lru < 0) || slot.lru_rank < _image_slots[static_cast<std::size_t>(lru)].lru_rank)
            lru = s;
    }
    const uint16_t slot_index = (target >= 0) ? static_cast<uint16_t>(target)
                                              : static_cast<uint16_t>(lru >= 0 ? lru : 0);
    auto& slot = _image_slots[slot_index];
    // Allocate in the LVGL pool so the raw buffer is classified as an image
    // source by the draw path (lv_mem_alloc pool range); PSRAM-backed on
    // the T-Deck. Allocation failure leaves the LRU intact.
    uint16_t* pixels = static_cast<uint16_t*>(
        lv_mem_alloc(pixel_count * sizeof(uint16_t)));
    if (!pixels) return false;
    std::memcpy(pixels, rgb565,
                static_cast<std::size_t>(pixel_count) * sizeof(uint16_t));
    if (slot.pixels) lv_mem_free(slot.pixels);
    slot.pixels = pixels;
    slot.pixel_count = static_cast<uint32_t>(pixel_count);
    slot.width = static_cast<uint16_t>(width);
    slot.height = static_cast<uint16_t>(height);
    slot.tag = image_index;
    slot.url_hash = image_url_hash(url.data(), url.size());
    slot.valid = true;
    slot.lru_rank = ++_image_slot_rank;
    // A decode reveals the intrinsic size; reflow the layout now so the
    // placeholder row becomes the correctly sized image box. Failure keeps
    // the previous layout (placeholder stays visible) and returns false.
    if (!relayout_images()) return false;
    return true;
}

bool NomadNetScreen::relayout_images() {
    if (!_page.has_image_blocks()) return true; // nothing to reflow
    const int32_t logical = _logical_scroll;
    // layout_page() rebuilds layout, checkpoints, focus and clears selection
    // state; the image blocks re-measure against their newly known intrinsic
    // sizes. Preserve the logical scroll so the viewport does not jump.
    bool ok = false;
    try { ok = layout_page(); } catch (const std::bad_alloc&) { ok = false; }
    if (!ok) return false;
    scroll_to_logical(logical, LV_ANIM_OFF);
    lv_obj_invalidate(_content);
    return true;
}

#ifdef PYXIS_TEST_HOOKS
void NomadNetScreen::test_image_state_dump() const {
    Serial.printf("T:IMG_STATE images=%u\n",
                  static_cast<unsigned>(_page.images().size()));
    for (uint16_t s = 0; s < MAX_DECODED_IMAGE_SLOTS; ++s) {
        const auto& slot = _image_slots[s];
        Serial.printf("T:SLOT %u valid=%d tag=%u w=%u h=%u rank=%u\n",
                      static_cast<unsigned>(s),
                      slot.valid ? 1 : 0,
                      static_cast<unsigned>(slot.tag),
                      static_cast<unsigned>(slot.width),
                      static_cast<unsigned>(slot.height),
                      static_cast<unsigned>(slot.lru_rank));
    }
}
#endif

bool NomadNetScreen::prepare_partial_request(
        const NomadNet::PartialRequest& request,
        NomadNet::PartialController& controller,
        NomadNet::FormEncodeResult& result) const {
    result = NomadNet::FormEncodeResult::INVALID_STATE;
    if (request.partial_index >= _page.partials().size()) return false;
    const auto& partial = _page.partials()[request.partial_index];
    if (partial.descriptor_hash != request.descriptor_hash) return false;
    const auto selectors = _page.partial_selectors(partial);
    if ((!selectors.data() && !selectors.empty()) ||
            selectors.size() > NomadNet::FormState::MAX_SELECTOR_BYTES)
        return false;
    NomadNet::ExternalVector<uint8_t> encoded;
    try {
        NomadNet::FormState snapshot = _form_state;
        if (_field_editor && _editing_field >= 0 &&
                static_cast<std::size_t>(_editing_field) < snapshot.fields().size()) {
            const char* value = lv_textarea_get_text(_field_editor);
            const std::size_t size = value ? std::strlen(value) : 0;
            if (!snapshot.set_value(static_cast<uint16_t>(_editing_field), value, size)) {
                result = NomadNet::FormEncodeResult::VALUE_TOO_LARGE;
                return false;
            }
        }
        result = snapshot.encode(
            std::string(selectors.data(), selectors.size()), encoded);
        if (result != NomadNet::FormEncodeResult::OK) return false;
        const bool prepared = controller.prepare(
            request, _page, encoded.data(), encoded.size());
        NomadNet::clear_encoded_form(encoded);
        if (!prepared) result = NomadNet::FormEncodeResult::INVALID_STATE;
        return prepared;
    } catch (const std::bad_alloc&) {
        NomadNet::clear_encoded_form(encoded);
        result = NomadNet::FormEncodeResult::ALLOCATION_FAILED;
        return false;
    }
}

bool NomadNetScreen::partial_id_matches(std::size_t partial_index,
                                        const char* id,
                                        std::size_t id_size) const {
    if ((!id && id_size != 0) || partial_index >= _page.partials().size())
        return false;
    const auto value = _page.partial_id(_page.partials()[partial_index]);
    return value.size() == id_size &&
        (id_size == 0 || std::memcmp(value.data(), id, id_size) == 0);
}

NomadNet::PartialReplaceResult NomadNetScreen::apply_partial_fragment(
        const NomadNet::PartialRequest& request,
        const NomadNet::Document& fragment,
        const NomadNet::PartialController& controller) {
    if (!controller.matches(request, _page))
        return NomadNet::PartialReplaceResult::INVALID_PARTIAL;

    NomadNet::CompactPage candidate_page;
    const auto replacement = candidate_page.assign_replacing_partial(
        _page, request.partial_index, fragment,
        NomadNet::CompactPage::MAX_ARENA_BYTES);
    if (replacement != NomadNet::PartialReplaceResult::APPLIED)
        return replacement;

    NomadNet::FormState candidate_form;
    if (!candidate_form.assign_preserving(candidate_page, _form_state))
        return NomadNet::PartialReplaceResult::ALLOCATION_FAILED;

    auto map_field = [&](int16_t old_index) -> int16_t {
        if (old_index < 0 ||
                static_cast<std::size_t>(old_index) >= _form_state.fields().size())
            return -1;
        const auto& old = _form_state.fields()[old_index];
        auto matches_identity = [&](const NomadNet::FormState::FieldState& candidate) {
            if (candidate.type != old.type ||
                    candidate.partial_region_index != old.partial_region_index ||
                    candidate.name_length != old.name_length ||
                    (old.name_length != 0 && std::memcmp(
                        candidate.name.data(), old.name.data(), old.name_length) != 0))
                return false;
            if (old.type != NomadNet::FormFieldType::RADIO &&
                    old.type != NomadNet::FormFieldType::CHECKBOX)
                return true;
            return candidate.value_length == old.value_length &&
                (old.value_length == 0 || std::memcmp(
                    candidate.value.data(), old.value.data(), old.value_length) == 0);
        };
        std::size_t occurrence = 0;
        for (std::size_t index = 0; index < static_cast<std::size_t>(old_index); ++index) {
            const auto& prior = _form_state.fields()[index];
            if (matches_identity(prior))
                ++occurrence;
        }
        std::size_t seen = 0;
        for (std::size_t index = 0; index < candidate_form.fields().size(); ++index) {
            const auto& candidate = candidate_form.fields()[index];
            if (!matches_identity(candidate)) continue;
            if (seen++ == occurrence) return static_cast<int16_t>(index);
        }
        return -1;
    };
    auto map_link = [&](int16_t old_index) -> int16_t {
        if (old_index < 0 ||
                static_cast<std::size_t>(old_index) >= _page.links().size())
            return -1;
        const auto old_target = _page.target(static_cast<std::size_t>(old_index));
        const int16_t old_region =
            _page.links()[static_cast<std::size_t>(old_index)].partial_region_index;
        std::size_t occurrence = 0;
        for (std::size_t index = 0; index < static_cast<std::size_t>(old_index); ++index) {
            const auto prior = _page.target(index);
            if (_page.links()[index].partial_region_index == old_region &&
                    prior.size() == old_target.size() &&
                    (old_target.empty() || std::memcmp(
                        prior.data(), old_target.data(), old_target.size()) == 0))
                ++occurrence;
        }
        std::size_t seen = 0;
        for (std::size_t index = 0; index < candidate_page.links().size(); ++index) {
            const auto candidate = candidate_page.target(index);
            if (candidate_page.links()[index].partial_region_index != old_region ||
                    candidate.size() != old_target.size() ||
                    (!old_target.empty() && std::memcmp(
                        candidate.data(), old_target.data(), old_target.size()) != 0))
                continue;
            if (seen++ == occurrence) return static_cast<int16_t>(index);
        }
        return -1;
    };
    const int16_t mapped_selected_link = map_link(_selected_link);
    const int16_t mapped_selected_field = map_field(_selected_field);
    const int16_t mapped_editing_field = map_field(_editing_field);
    int16_t fallback_link = -1;
    int16_t fallback_field = -1;
    auto map_focus_at = [&](std::size_t index) {
        if (index >= _focus_order.size()) return false;
        const auto& target = _focus_order[index];
        if (target.field) fallback_field = map_field(
            static_cast<int16_t>(target.index));
        else fallback_link = map_link(static_cast<int16_t>(target.index));
        return fallback_field >= 0 || fallback_link >= 0;
    };
    if (mapped_selected_link < 0 && mapped_selected_field < 0 &&
            _selected_focus >= 0 &&
            static_cast<std::size_t>(_selected_focus) < _focus_order.size()) {
        for (std::size_t index = static_cast<std::size_t>(_selected_focus) + 1;
                index < _focus_order.size(); ++index)
            if (map_focus_at(index)) break;
        if (fallback_link < 0 && fallback_field < 0) {
            for (std::size_t index = static_cast<std::size_t>(_selected_focus);
                    index-- > 0;)
                if (map_focus_at(index)) break;
        }
    }

    bool has_top_anchor = false;
    int16_t old_top_region = -1;
    std::size_t old_top_region_ordinal = 0;
    int32_t old_top_intra = 0;
    const LayoutCheckpoint* old_top = nullptr;
    for (const auto& checkpoint : _layout_checkpoints) {
        if (checkpoint.y <= _logical_scroll &&
                (!old_top || checkpoint.y >= old_top->y))
            old_top = &checkpoint;
    }
    if (old_top && old_top->block_index < _page.blocks().size()) {
        has_top_anchor = true;
        old_top_region = _page.blocks()[old_top->block_index].partial_region_index;
        for (std::size_t index = 0; index < old_top->block_index; ++index)
            if (_page.blocks()[index].partial_region_index == old_top_region)
                ++old_top_region_ordinal;
        old_top_intra = std::max<int32_t>(0, _logical_scroll - old_top->y);
    }

    if (_field_editor && mapped_editing_field >= 0) {
        const char* value = lv_textarea_get_text(_field_editor);
        const std::size_t size = value ? std::strlen(value) : 0;
        if (!candidate_form.set_value(
                static_cast<uint16_t>(mapped_editing_field), value, size))
            return NomadNet::PartialReplaceResult::LIMIT_EXCEEDED;
    }

    auto old_page = std::move(_page);
    auto old_form = std::move(_form_state);
    auto old_page_layout = std::move(_page_layout);
    auto old_line_layout = std::move(_line_layout);
    auto old_checkpoints = std::move(_layout_checkpoints);
    auto old_link_y = std::move(_link_y);
    auto old_link_bottom = std::move(_link_bottom);
    auto old_field_y = std::move(_field_y);
    auto old_field_bottom = std::move(_field_bottom);
    auto old_focus = std::move(_focus_order);
    const int32_t old_page_height = _page_height;
    const int32_t old_physical_extent = _physical_extent;
    const int32_t old_logical_scroll = _logical_scroll;
    const int32_t old_widget_scroll = lv_obj_get_scroll_y(_content);
    const int32_t old_window_top = _layout_window_top;
    const int32_t old_window_bottom = _layout_window_bottom;
    const TableLayoutObservation old_table_layout = _table_layout;
    const int16_t old_selected_link = _selected_link;
    const int16_t old_selected_field = _selected_field;
    const int16_t old_selected_focus = _selected_focus;
    const int16_t old_editing_field = _editing_field;

    _page = std::move(candidate_page);
    _form_state = std::move(candidate_form);
    bool laid_out = false;
    try { laid_out = layout_page(); } catch (const std::bad_alloc&) { laid_out = false; }
    auto rollback = [&]() {
        _page = std::move(old_page);
        _form_state = std::move(old_form);
        _page_layout = std::move(old_page_layout);
        _line_layout = std::move(old_line_layout);
        _layout_checkpoints = std::move(old_checkpoints);
        _link_y = std::move(old_link_y);
        _link_bottom = std::move(old_link_bottom);
        _field_y = std::move(old_field_y);
        _field_bottom = std::move(old_field_bottom);
        _focus_order = std::move(old_focus);
        _page_height = old_page_height;
        _physical_extent = old_physical_extent;
        _logical_scroll = old_logical_scroll;
        _layout_window_top = old_window_top;
        _layout_window_bottom = old_window_bottom;
        _table_layout = old_table_layout;
        _selected_link = old_selected_link;
        _selected_field = old_selected_field;
        _selected_focus = old_selected_focus;
        _editing_field = old_editing_field;
        _transaction_scroll_restore = true;
        lv_obj_scroll_to_y(_content, old_widget_scroll, LV_ANIM_OFF);
        _transaction_scroll_restore = false;
    };
    if (!laid_out) {
        rollback();
        return NomadNet::PartialReplaceResult::ALLOCATION_FAILED;
    }


    _selected_link = mapped_selected_link >= 0 ? mapped_selected_link : fallback_link;
    _selected_field = mapped_selected_field >= 0 ? mapped_selected_field : fallback_field;
    _selected_focus = -1;
    if (_selected_field >= 0 || _selected_link >= 0) {
        for (std::size_t index = 0; index < _focus_order.size(); ++index) {
            const bool matches_field = _selected_field >= 0 &&
                _focus_order[index].field && _focus_order[index].index ==
                    static_cast<uint16_t>(_selected_field);
            const bool matches_link = _selected_field < 0 &&
                _selected_link >= 0 && !_focus_order[index].field &&
                _focus_order[index].index ==
                    static_cast<uint16_t>(_selected_link);
            if (matches_field || matches_link) {
                _selected_focus = static_cast<int16_t>(index);
                break;
            }
        }
    }

    if (_field_editor && mapped_editing_field >= 0)
        _editing_field = mapped_editing_field;
    if (!scroll_to_logical(old_logical_scroll, LV_ANIM_OFF)) {
        rollback();
        lv_obj_invalidate(_content);
        return NomadNet::PartialReplaceResult::ALLOCATION_FAILED;
    }
    if (has_top_anchor) {
        std::size_t mapped_block = _page.blocks().size();
        std::size_t region_top = _page.blocks().size();
        bool anchor_survived = false;
        std::size_t seen = 0;
        for (std::size_t index = 0; index < _page.blocks().size(); ++index) {
            if (_page.blocks()[index].partial_region_index != old_top_region) continue;
            if (region_top == _page.blocks().size()) region_top = index;
            if (seen++ == old_top_region_ordinal) {
                mapped_block = index;
                anchor_survived = true;
                break;
            }
        }
        if (mapped_block == _page.blocks().size()) mapped_block = region_top;
        if (mapped_block < _page.blocks().size()) {
            for (const auto& checkpoint : _layout_checkpoints) {
                if (checkpoint.block_index != mapped_block) continue;
                const int32_t mapped_intra = anchor_survived ? old_top_intra : 0;
                if (!scroll_to_logical(checkpoint.y + mapped_intra, LV_ANIM_OFF)) {
                    rollback();
                    lv_obj_invalidate(_content);
                    return NomadNet::PartialReplaceResult::ALLOCATION_FAILED;
                }
                break;
            }
        }
    }
    if (_field_editor && mapped_editing_field < 0)
        finish_field_edit(false);
    ++_form_generation;
    lv_obj_refresh_self_size(_content);
    lv_obj_invalidate(_content);
    return NomadNet::PartialReplaceResult::APPLIED;
}

bool NomadNetScreen::layout_page(){
    _table_layout=TableLayoutObservation{};
    _page_layout.clear();
    _line_layout.clear();
    _layout_checkpoints.clear();
    _link_y.assign(_page.links().size(),-1);
    _link_bottom.assign(_page.links().size(),-1);
    _field_y.assign(_page.fields().size(),-1);
    _field_bottom.assign(_page.fields().size(),-1);
    _focus_order.clear();
    _page_layout.reserve(MAX_WINDOW_FRAGMENTS);
    _line_layout.reserve(NomadNet::DocumentParser::MAX_RUNS_PER_LINE);
    _layout_checkpoints.reserve(_page.blocks().size());
    const int32_t viewport=std::max<int32_t>(1,lv_obj_get_content_height(_content));
    if(!layout_from(0,0,0,viewport*3,true))return false;
    _focus_order.reserve(_page.links().size()+_page.fields().size());
    NomadNet::ExternalVector<uint16_t> link_order(_page.links().size(),UINT16_MAX);
    NomadNet::ExternalVector<uint16_t> field_order(_page.fields().size(),UINT16_MAX);
    uint16_t source_order=0;
    for(const auto& run:_page.runs()){
        if(run.link_index>=0&&static_cast<std::size_t>(run.link_index)<link_order.size()&&
           link_order[run.link_index]==UINT16_MAX)link_order[run.link_index]=source_order++;
        if(run.field_index>=0&&static_cast<std::size_t>(run.field_index)<field_order.size()&&
           field_order[run.field_index]==UINT16_MAX)field_order[run.field_index]=source_order++;
    }
    for(std::size_t i=0;i<_link_y.size();++i)
        if(_link_y[i]>=0)_focus_order.push_back(FocusTarget{
            static_cast<uint16_t>(i),_link_y[i],_link_bottom[i],false,link_order[i]});
    for(std::size_t i=0;i<_field_y.size();++i)
        if(_field_y[i]>=0)_focus_order.push_back(FocusTarget{
            static_cast<uint16_t>(i),_field_y[i],_field_bottom[i],true,field_order[i]});
    std::stable_sort(_focus_order.begin(),_focus_order.end(),
        [](const FocusTarget& left,const FocusTarget& right){
            if(left.y!=right.y)return left.y<right.y;
            if(left.order!=right.order)return left.order<right.order;
            if(left.field!=right.field)return left.field;
            return left.index<right.index;
        });
    _selected_focus=-1;_selected_link=-1;_selected_field=-1;
    _physical_extent=std::min<int32_t>(_page_height,MAX_PHYSICAL_SCROLL_EXTENT);
    _logical_scroll=0;
    _layout_window_top=0;
    _layout_window_bottom=std::min<int32_t>(_page_height,viewport*3);
    return true;
}

bool NomadNetScreen::append_line_fragment(const LayoutFragment& fragment){
    if(!_line_layout.empty()){
        auto& previous=_line_layout.back();
        const uint32_t combined=static_cast<uint32_t>(previous.byte_length)+fragment.byte_length;
        if(previous.link_index==fragment.link_index&&previous.field_index==fragment.field_index&&
           previous.large_font==fragment.large_font&&combined<256&&NomadNet::VirtualViewport::can_coalesce(previous.run_index,
               previous.byte_offset,previous.byte_length,fragment.run_index,fragment.byte_offset)){
            previous.byte_length=static_cast<uint16_t>(combined);
            previous.width=static_cast<int16_t>(previous.width+fragment.width);
            return true;
        }
    }
    if(_line_layout.size()>=NomadNet::DocumentParser::MAX_RUNS_PER_LINE)return false;
    _line_layout.push_back(fragment);
    return true;
}

bool NomadNetScreen::commit_line(int32_t line_y,int16_t line_height,
                                 NomadNet::Alignment alignment,int16_t indent,
                                 int16_t available,uint8_t heading_level,
                                 int32_t window_top,int32_t window_bottom){
    if(_line_layout.empty()&&heading_level==0)return true;
    int16_t line_width=0;
    for(const auto& fragment:_line_layout)
        line_width=std::max<int16_t>(line_width,fragment.x+fragment.width-indent);
    const int16_t shift=alignment==NomadNet::Alignment::CENTER?(available-line_width)/2:
        alignment==NomadNet::Alignment::RIGHT?available-line_width:0;
    if(line_y+line_height>=window_top&&line_y<window_bottom){
        const int32_t relative_y=line_y-window_top;
        if(relative_y<INT16_MIN||relative_y>INT16_MAX)return false;
        if(_line_layout.empty()){
            if(_page_layout.size()>=MAX_WINDOW_FRAGMENTS)return false;
            LayoutFragment band(UINT16_MAX,0,0,-1,0,static_cast<int16_t>(relative_y),
                                0,line_height,false,heading_level==1);
            band.set_heading(heading_level,true);
            _page_layout.push_back(band);
        }else{
            bool starts_band=true;
            for(auto fragment:_line_layout){
                if(_page_layout.size()>=MAX_WINDOW_FRAGMENTS)return false;
                if(shift>0)fragment.x=static_cast<int16_t>(fragment.x+shift);
                fragment.y=static_cast<int16_t>(relative_y);
                if(heading_level!=0)fragment.set_heading(heading_level,starts_band);
                starts_band=false;
                _page_layout.push_back(fragment);
            }
        }
    }
    _line_layout.clear();
    return true;
}

bool NomadNetScreen::layout_table_cell(const NomadNet::CompactPage::TableCellRecord& cell,
                                       int16_t left,int16_t available,int32_t top,
                                       int32_t window_top,int32_t window_bottom,
                                       bool emit,int32_t& height){
    if(available<1||cell.first_run>_page.runs().size()||
       cell.run_count>_page.runs().size()-cell.first_run)return false;
    int16_t x=left;
    int16_t line_h=16;
    int32_t line_y=top;
    bool line_started=false;
    _line_layout.clear();
    auto finish_line=[&](){
        if(emit){
            if(!commit_line(line_y,line_h,cell.alignment,left,available,0,
                            window_top,window_bottom))return false;
        }else _line_layout.clear();
        line_y+=line_h;
        x=left;
        line_h=16;
        line_started=false;
        return true;
    };
    for(uint16_t r=0;r<cell.run_count;++r){
        const uint16_t run_index=static_cast<uint16_t>(cell.first_run+r);
        const auto& run=_page.runs()[run_index];
        const auto text=_page.text(run);
        const lv_font_t* font=page_run_font(run,false);
        const int16_t run_h=static_cast<int16_t>(font->line_height+3);
        if(run.field_index>=0&&static_cast<std::size_t>(run.field_index)<_page.fields().size()){
            const auto& field=_page.fields()[run.field_index];
            const int16_t space_width=std::max<int16_t>(1,static_cast<int16_t>(
                lv_txt_get_width(" ",1,font,0,LV_TEXT_FLAG_NONE)));
            int32_t requested=0;
            if(field.type==NomadNet::FormFieldType::TEXT||field.type==NomadNet::FormFieldType::PASSWORD){
                requested=std::max<int32_t>(48,static_cast<int32_t>(field.width)*space_width+8);
            }else{
                const auto label=_page.field_label(run.field_index);
                const char* prefix=field.type==NomadNet::FormFieldType::RADIO?"( ) ":"[ ] ";
                const int16_t prefix_width=static_cast<int16_t>(
                    lv_txt_get_width(prefix,4,font,0,LV_TEXT_FLAG_NONE));
                requested=8+prefix_width+lv_txt_get_width(
                    label.data(),static_cast<uint32_t>(label.size()),font,0,LV_TEXT_FLAG_NONE);
            }
            const int16_t field_width=static_cast<int16_t>(std::max<int32_t>(1,
                std::min<int32_t>(available,requested)));
            const int16_t field_height=std::max<int16_t>(20,run_h+4);
            if(x>left&&x+field_width>left+available){if(!finish_line())return false;}
            line_h=std::max(line_h,field_height);
            if(emit&&static_cast<std::size_t>(run.field_index)<_field_y.size()){
                if(_field_y[run.field_index]<0)_field_y[run.field_index]=line_y;
                _field_bottom[run.field_index]=std::max(_field_bottom[run.field_index],line_y+field_height);
            }
            if(emit){
                LayoutFragment field_fragment(run_index,0,0,-1,x,0,field_width,field_height,false);
                field_fragment.field_index=run.field_index;
                if(!append_line_fragment(field_fragment))return false;
            }
            x=static_cast<int16_t>(x+field_width);line_started=true;
            continue;
        }
        line_h=std::max(line_h,run_h);
        std::size_t offset=0;
        while(offset<text.size()){
            std::size_t end=offset;
            const bool whitespace=text[offset]==' '||text[offset]=='\t';
            while(end<text.size()&&((text[end]==' '||text[end]=='\t')==whitespace)&&end-offset<255)++end;
            if(end==offset)++end;
            int16_t fragment_w=static_cast<int16_t>(lv_txt_get_width(text.data()+offset,
                static_cast<uint32_t>(end-offset),font,0,LV_TEXT_FLAG_NONE));
            if(whitespace&&x+fragment_w>left+available){offset=end;continue;}
            if(!whitespace&&x>left&&x+fragment_w>left+available){
                if(!finish_line())return false;
                line_h=run_h;
            }
            if(fragment_w>available){
                end=offset;
                fragment_w=0;
                while(end<text.size()&&end-offset<255){
                    const std::size_t previous=end;
                    uint32_t next_index=static_cast<uint32_t>(end);
                    _lv_txt_encoded_next(text.data(),&next_index);
                    end=next_index;
                    const int16_t candidate=static_cast<int16_t>(lv_txt_get_width(text.data()+offset,
                        static_cast<uint32_t>(end-offset),font,0,LV_TEXT_FLAG_NONE));
                    if(candidate>available&&previous>offset){end=previous;break;}
                    fragment_w=candidate;
                }
            }
            if(!(whitespace&&x==left)){
                if(emit){
                    if(run.link_index>=0&&static_cast<std::size_t>(run.link_index)<_link_y.size()){
                        if(_link_y[run.link_index]<0)_link_y[run.link_index]=line_y;
                        _link_bottom[run.link_index]=std::max(_link_bottom[run.link_index],line_y+run_h);
                    }
                    if(!append_line_fragment(LayoutFragment(run_index,static_cast<uint16_t>(offset),
                        static_cast<uint16_t>(end-offset),run.link_index,x,0,fragment_w,run_h,false)))return false;
                }
                x=static_cast<int16_t>(x+fragment_w);
                line_started=true;
            }
            offset=end;
        }
    }
    if(line_started||line_y==top){
        if(!finish_line())return false;
    }
    height=std::max<int32_t>(16,line_y-top);
    return true;
}

bool NomadNetScreen::layout_table_fit(const NomadNet::CompactPage::TableRecord& table,
                                      const int16_t* column_widths,int16_t table_fit_width,
                                      int32_t& y,int32_t window_top,int32_t window_bottom){
    constexpr int16_t content_width=304;
    int16_t table_left=table.alignment==NomadNet::Alignment::CENTER?
        static_cast<int16_t>((content_width-table_fit_width)/2):
        table.alignment==NomadNet::Alignment::RIGHT?
            static_cast<int16_t>(content_width-table_fit_width):0;
    for(uint16_t row=0;row<table.row_count;++row){
        int32_t row_height=16;
        for(uint8_t column=0;column<table.column_count;++column){
            const std::size_t cell_index=table.first_cell+
                static_cast<std::size_t>(row)*table.column_count+column;
            if(cell_index>=_page.table_cells().size())return false;
            int32_t cell_height=0;
            if(!layout_table_cell(_page.table_cells()[cell_index],0,
                    std::max<int16_t>(1,column_widths[column]-8),0,
                    window_top,window_bottom,false,cell_height))return false;
            row_height=std::max(row_height,cell_height+6);
        }
        int16_t cell_left=table_left;
        for(uint8_t column=0;column<table.column_count;++column){
            const std::size_t cell_index=table.first_cell+
                static_cast<std::size_t>(row)*table.column_count+column;
            if(y+row_height>=window_top&&y<window_bottom){
                if(_page_layout.size()>=MAX_WINDOW_FRAGMENTS)return false;
                LayoutFragment box(UINT16_MAX,0,0,-1,cell_left,
                    static_cast<int16_t>(y-window_top),column_widths[column],
                    static_cast<int16_t>(row_height),false);
                box.table_cell=true;
                box.table_header=row==0;
                _page_layout.push_back(box);
            }
            int32_t ignored=0;
            if(!layout_table_cell(_page.table_cells()[cell_index],
                    static_cast<int16_t>(cell_left+4),
                    std::max<int16_t>(1,column_widths[column]-8),y+3,
                    window_top,window_bottom,true,ignored))return false;
            cell_left=static_cast<int16_t>(cell_left+column_widths[column]);
        }
        y+=row_height;
    }
    y+=3;
    return true;
}

bool NomadNetScreen::layout_table_reflow(const NomadNet::CompactPage::TableRecord& table,
                                         int32_t& y,int32_t window_top,int32_t window_bottom){
    constexpr int16_t card_width=304;
    constexpr int16_t text_left=4;
    constexpr int16_t text_width=296;
    const uint16_t data_rows=table.row_count>1?static_cast<uint16_t>(table.row_count-1):1;
    for(uint16_t data_row=0;data_row<data_rows;++data_row){
        for(uint8_t column=0;column<table.column_count;++column){
            const std::size_t header_index=table.first_cell+column;
            const std::size_t value_index=table.row_count>1?
                table.first_cell+static_cast<std::size_t>(data_row+1)*table.column_count+column:
                header_index;
            if(header_index>=_page.table_cells().size()||value_index>=_page.table_cells().size())return false;
            const std::size_t pair_count=table.row_count>1?2:1;
            const std::size_t indices[2]={header_index,value_index};
            for(std::size_t part=0;part<pair_count;++part){
                int32_t cell_height=0;
                if(!layout_table_cell(_page.table_cells()[indices[part]],text_left,text_width,0,
                        window_top,window_bottom,false,cell_height))return false;
                const int32_t box_height=cell_height+6;
                if(y+box_height>=window_top&&y<window_bottom){
                    if(_page_layout.size()>=MAX_WINDOW_FRAGMENTS)return false;
                    LayoutFragment box(UINT16_MAX,0,0,-1,0,
                        static_cast<int16_t>(y-window_top),card_width,
                        static_cast<int16_t>(box_height),false);
                    box.table_cell=true;
                    box.table_header=part==0;
                    _page_layout.push_back(box);
                }
                int32_t ignored=0;
                if(!layout_table_cell(_page.table_cells()[indices[part]],text_left,text_width,y+3,
                        window_top,window_bottom,true,ignored))return false;
                y+=box_height;
            }
        }
        y+=4;
    }
    return true;
}

bool NomadNetScreen::layout_table(const NomadNet::CompactPage::BlockRecord& block,
                                  int32_t& y,int32_t window_top,int32_t window_bottom){
    constexpr int16_t content_width=304;
    if(block.table_index<0||static_cast<std::size_t>(block.table_index)>=_page.tables().size())return false;
    const auto& table=_page.tables()[block.table_index];
    if(table.column_count==0||table.column_count>NomadNet::DocumentParser::MAX_TABLE_COLUMNS||
       table.row_count==0)return false;
    int16_t column_widths[NomadNet::DocumentParser::MAX_TABLE_COLUMNS]={0};
    const int16_t minimum_width=static_cast<int16_t>(lv_txt_get_width(
        "   ",3,&nomadnet_font_12,0,LV_TEXT_FLAG_NONE)+8);
    for(uint8_t column=0;column<table.column_count;++column)column_widths[column]=minimum_width;
    for(uint16_t row=0;row<table.row_count;++row){
        for(uint8_t column=0;column<table.column_count;++column){
            const std::size_t cell_index=table.first_cell+
                static_cast<std::size_t>(row)*table.column_count+column;
            if(cell_index>=_page.table_cells().size())return false;
            const auto& cell=_page.table_cells()[cell_index];
            if(cell.first_run>_page.runs().size()||cell.run_count>_page.runs().size()-cell.first_run)return false;
            int32_t measured=8;
            for(uint16_t r=0;r<cell.run_count;++r){
                const auto& run=_page.runs()[cell.first_run+r];
                if(run.field_index>=0&&static_cast<std::size_t>(run.field_index)<_page.fields().size()){
                    const auto& field=_page.fields()[run.field_index];
                    const auto* font=page_run_font(run,false);
                    int32_t field_width=0;
                    if(field.type==NomadNet::FormFieldType::TEXT||
                       field.type==NomadNet::FormFieldType::PASSWORD){
                        field_width=std::max<int32_t>(48,static_cast<int32_t>(field.width)*
                            std::max<int32_t>(1,lv_txt_get_width(" ",1,font,0,LV_TEXT_FLAG_NONE))+8);
                    }else{
                        const auto label=_page.field_label(run.field_index);
                        const char* prefix=field.type==NomadNet::FormFieldType::RADIO?"( ) ":"[ ] ";
                        const int16_t prefix_width=static_cast<int16_t>(
                            lv_txt_get_width(prefix,4,font,0,LV_TEXT_FLAG_NONE));
                        const int32_t requested=8+prefix_width+lv_txt_get_width(
                            label.data(),static_cast<uint32_t>(label.size()),font,0,LV_TEXT_FLAG_NONE);
                        field_width=requested;
                    }
                    measured+=field_width;
                }else{
                    const auto text=_page.text(run);
                    measured+=lv_txt_get_width(text.data(),static_cast<uint32_t>(text.size()),
                        page_run_font(run,false),0,LV_TEXT_FLAG_NONE);
                }
                measured=std::min<int32_t>(measured,INT16_MAX);
            }
            column_widths[column]=std::max<int16_t>(column_widths[column],
                static_cast<int16_t>(measured));
        }
    }
    int32_t natural_width=0;
    for(uint8_t column=0;column<table.column_count;++column)natural_width+=column_widths[column];
    const int32_t space_width=std::max<int32_t>(1,lv_txt_get_width(
        " ",1,&nomadnet_font_12,0,LV_TEXT_FLAG_NONE));
    const int32_t metadata_width=std::min<int32_t>(content_width,
        static_cast<int32_t>(table.max_width)*space_width);
    const int32_t structural_minimum=static_cast<int32_t>(minimum_width)*table.column_count;
    const auto tier=NomadNet::choose_table_layout(
        structural_minimum,natural_width,content_width);
    const int32_t table_top=y;
    if(tier!=NomadNet::TableLayoutTier::STACKED){
        const int16_t target_width=static_cast<int16_t>(std::max<int32_t>(
            structural_minimum,std::min<int32_t>(metadata_width,content_width)));
        const int16_t table_fit_width=NomadNet::fit_table_columns(
            column_widths,table.column_count,minimum_width,target_width);
        if(!layout_table_fit(table,column_widths,table_fit_width,y,window_top,window_bottom))return false;
        if(!_table_layout.valid){
            const int16_t left=table.alignment==NomadNet::Alignment::CENTER?
                static_cast<int16_t>((content_width-table_fit_width)/2):
                table.alignment==NomadNet::Alignment::RIGHT?
                    static_cast<int16_t>(content_width-table_fit_width):0;
            _table_layout={tier,left,table_top,table_fit_width,y-table_top,
                           table.column_count,0,true};
        }
        return true;
    }
    if(!layout_table_reflow(table,y,window_top,window_bottom))return false;
    if(!_table_layout.valid){
        const uint16_t rows=table.row_count>1?static_cast<uint16_t>(table.row_count-1):1;
        _table_layout={tier,0,table_top,content_width,y-table_top,0,
                       static_cast<uint16_t>(rows*table.column_count),true};
    }
    return true;
}

bool NomadNetScreen::layout_from(std::size_t start_block,int32_t start_y,
                                 int32_t window_top,int32_t window_bottom,
                                 bool build_index){
    constexpr int16_t width=304;
    int32_t y=start_y;
    for(std::size_t block_index=start_block;block_index<_page.blocks().size();++block_index){
        if(!build_index&&y>=window_bottom)break;
        if(build_index){
            if(_layout_checkpoints.size()>=NomadNet::CompactPage::MAX_BLOCKS)return false;
            _layout_checkpoints.push_back(LayoutCheckpoint{
                static_cast<uint16_t>(block_index),y});
        }
        const auto& block=_page.blocks()[block_index];
        if(block.type==NomadNet::BlockType::DIVIDER){
            const int16_t divider_height=static_cast<int16_t>(nomadnet_font_12.line_height);
            if(y+divider_height>=window_top&&y<window_bottom){
                if(_page_layout.size()>=MAX_WINDOW_FRAGMENTS)return false;
                LayoutFragment divider(0,0,0,-1,0,
                    static_cast<int16_t>(y-window_top),width,divider_height,true);
                divider.divider_codepoint=block.divider_codepoint;
                _page_layout.push_back(divider);
            }
            y+=divider_height;continue;
        }
        if(block.type==NomadNet::BlockType::TABLE){
            if(!layout_table(block,y,window_top,window_bottom))return false;
            continue;
        }
        if(block.type==NomadNet::BlockType::IMAGE){
            const bool has_record=block.image_index>=0&&
                static_cast<std::size_t>(block.image_index)<_page.images().size();
            uint16_t native_w=0,native_h=0;
            const uint16_t* decoded=has_record?
                decoded_image(block.image_index,native_w,native_h):nullptr;
            const auto& record=has_record?_page.images()[block.image_index]:
                NomadNet::CompactPage::ImageRecord{};
            const int32_t viewport=std::max<int32_t>(1,lv_obj_get_content_height(_content));
            const auto placement=compute_image_placement(
                0,static_cast<uint32_t>(width),
                static_cast<uint32_t>(std::max<int32_t>(1,viewport*3)),
                record,decoded?native_w:0,decoded?native_h:0);
            if(y+placement.height>=window_top&&y<window_bottom){
                if(_page_layout.size()>=MAX_WINDOW_FRAGMENTS)return false;
                // run_index carries the placeholder run (the "[Image: ...]"
                // text) so the fallback path can draw it before decode.
                const uint16_t placeholder_run=block.run_count!=0?
                    static_cast<uint16_t>(block.first_run):0;
                LayoutFragment fragment(placeholder_run,0,0,-1,
                    placement.x,static_cast<int16_t>(y-window_top),
                    placement.width,placement.height,false);
                fragment.image_index=block.image_index;
                fragment.image_align=static_cast<uint8_t>(record.align);
                fragment.image_decoded=decoded!=nullptr;
                _page_layout.push_back(fragment);
            }
            y+=placement.height;continue;
        }
        const bool heading=block.type==NomadNet::BlockType::HEADING;
        const bool has_runs=block.run_count!=0&&block.first_run<_page.runs().size();
        if(!heading&&!has_runs)continue;
        const uint8_t heading_level=heading?NomadNet::heading_display_level(block.depth):0;
        const bool large_heading=heading&&NomadNet::heading_uses_large_font(block.depth);
        int16_t indent=static_cast<int16_t>(std::min<unsigned>(block.depth,8)*4);
        NomadNet::CompactPage::RunRecord default_run{};
        const auto& first_run=has_runs?_page.runs()[block.first_run]:default_run;
        if(heading){
            const lv_font_t* indent_font=page_run_font(first_run,large_heading);
            const int16_t space_width=static_cast<int16_t>(lv_txt_get_width(
                " ",1,indent_font,0,LV_TEXT_FLAG_NONE));
            indent=static_cast<int16_t>(NomadNet::heading_indent_spaces(block.depth)*space_width);
        }
        const int16_t available=width-indent;
        int16_t x=indent;
        int16_t line_h=heading?static_cast<int16_t>(page_run_font(first_run,large_heading)->line_height+3):16;
        int32_t line_y=y;
        _line_layout.clear();
        for(uint16_t r=0;r<block.run_count&&block.first_run+r<_page.runs().size();++r){
            const uint16_t run_index=static_cast<uint16_t>(block.first_run+r);
            const auto& run=_page.runs()[run_index];
            const auto text=_page.text(run);
            const bool large=large_heading;
            const lv_font_t* font=page_run_font(run,large);
            const int16_t height=static_cast<int16_t>(font->line_height+3);
            if(run.field_index>=0&&static_cast<std::size_t>(run.field_index)<_page.fields().size()){
                const auto& field=_page.fields()[run.field_index];
                const int16_t space_width=std::max<int16_t>(1,static_cast<int16_t>(
                    lv_txt_get_width(" ",1,font,0,LV_TEXT_FLAG_NONE)));
                int32_t requested=0;
                if(field.type==NomadNet::FormFieldType::TEXT||field.type==NomadNet::FormFieldType::PASSWORD){
                    requested=static_cast<int32_t>(field.width)*space_width+8;
                    requested=std::max<int32_t>(48,requested);
                }else{
                    const auto label=_page.field_label(run.field_index);
                    const char* prefix=field.type==NomadNet::FormFieldType::RADIO?"( ) ":"[ ] ";
                    const int16_t prefix_width=static_cast<int16_t>(
                        lv_txt_get_width(prefix,4,font,0,LV_TEXT_FLAG_NONE));
                    requested=8+prefix_width+lv_txt_get_width(
                        label.data(),static_cast<uint32_t>(label.size()),font,0,LV_TEXT_FLAG_NONE);
                }
                const int16_t field_width=static_cast<int16_t>(std::max<int32_t>(1,
                    std::min<int32_t>(available,requested)));
                const int16_t field_height=std::max<int16_t>(20,height+4);
                if(x>indent&&x+field_width>indent+available){
                    if(!commit_line(line_y,line_h,block.alignment,indent,available,
                                    heading_level,window_top,window_bottom))return false;
                    y+=line_h;x=indent;line_h=field_height;line_y=y;
                }
                line_h=std::max(line_h,field_height);
                if(static_cast<std::size_t>(run.field_index)<_field_y.size()){
                    if(_field_y[run.field_index]<0)_field_y[run.field_index]=line_y;
                    _field_bottom[run.field_index]=std::max(_field_bottom[run.field_index],line_y+field_height);
                }
                LayoutFragment field_fragment(run_index,0,0,-1,x,0,field_width,field_height,false,large);
                field_fragment.field_index=run.field_index;
                if(!append_line_fragment(field_fragment))return false;
                x=static_cast<int16_t>(x+field_width);
                continue;
            }
            line_h=std::max(line_h,height);
            std::size_t offset=0;
            while(offset<text.size()){
                std::size_t end=offset;
                const bool whitespace=text[offset]==' '||text[offset]=='\t';
                while(end<text.size()&&((text[end]==' '||text[end]=='\t')==whitespace)&&end-offset<255)++end;
                if(end==offset)++end;
                int16_t fragment_w=static_cast<int16_t>(lv_txt_get_width(text.data()+offset,
                    static_cast<uint32_t>(end-offset),font,0,LV_TEXT_FLAG_NONE));
                if(whitespace&&x+fragment_w>indent+available){offset=end;continue;}
                if(!whitespace&&x>indent&&x+fragment_w>indent+available){
                    if(!commit_line(line_y,line_h,block.alignment,indent,available,
                                    heading_level,window_top,window_bottom))return false;
                    y+=line_h;x=indent;line_h=height;line_y=y;
                }
                if(fragment_w>available){
                    end=offset;
                    fragment_w=0;
                    while(end<text.size()&&end-offset<255){
                        const std::size_t previous=end;
                        uint32_t next_index=static_cast<uint32_t>(end);
                        _lv_txt_encoded_next(text.data(),&next_index);
                        end=next_index;
                        const int16_t candidate=static_cast<int16_t>(lv_txt_get_width(text.data()+offset,
                            static_cast<uint32_t>(end-offset),font,0,LV_TEXT_FLAG_NONE));
                        if(candidate>available&&previous>offset){end=previous;break;}
                        fragment_w=candidate;
                    }
                }
                if(!(whitespace&&x==indent&&!heading)){
                    if(run.link_index>=0&&static_cast<std::size_t>(run.link_index)<_link_y.size()){
                        if(_link_y[run.link_index]<0)_link_y[run.link_index]=line_y;
                        _link_bottom[run.link_index]=std::max(_link_bottom[run.link_index],line_y+height);
                    }
                    if(!append_line_fragment(LayoutFragment(run_index,static_cast<uint16_t>(offset),
                        static_cast<uint16_t>(end-offset),run.link_index,x,0,fragment_w,height,false,
                        large)))return false;
                    x=static_cast<int16_t>(x+fragment_w);
                }
                offset=end;
            }
        }
        if(!commit_line(line_y,line_h,block.alignment,indent,available,
                        heading_level,window_top,window_bottom))return false;
        y+=line_h+(heading?NomadNet::heading_bottom_spacing(block.depth):3);
    }
    if(build_index)_page_height=std::max<int32_t>(y,lv_obj_get_content_height(_content));
    return true;
}

bool NomadNetScreen::layout_window(int32_t logical_scroll){
    const int32_t viewport=std::max<int32_t>(1,lv_obj_get_content_height(_content));
    const int32_t window_top=NomadNet::VirtualViewport::window_top(logical_scroll,viewport);
    const int32_t window_bottom=NomadNet::VirtualViewport::window_bottom(
        logical_scroll,viewport,_page_height);
    if(window_top==_layout_window_top&&window_bottom==_layout_window_bottom)return true;
    std::size_t start_block=0;
    int32_t start_y=0;
    for(const auto& checkpoint:_layout_checkpoints){
        if(checkpoint.y>window_top)break;
        start_block=checkpoint.block_index;
        start_y=checkpoint.y;
    }
    _page_layout.clear();
    if(!layout_from(start_block,start_y,window_top,window_bottom,false))return false;
    _layout_window_top=window_top;
    _layout_window_bottom=window_bottom;
    return true;
}

int32_t NomadNetScreen::logical_scroll_from_widget()const{
    return NomadNet::VirtualViewport::logical_from_physical(
        lv_obj_get_scroll_y(_content),_page_height,
        lv_obj_get_content_height(_content),_physical_extent);
}

bool NomadNetScreen::scroll_to_logical(int32_t logical,lv_anim_enable_t animation){
#ifdef PYXIS_NOMADNET_TEST_HOOKS
    if(_test_scroll_fail_countdown==0){_test_scroll_fail_countdown=-1;return false;}
    if(_test_scroll_fail_countdown>0)--_test_scroll_fail_countdown;
#endif
    const int32_t viewport=std::max<int32_t>(1,lv_obj_get_content_height(_content));
    const int32_t logical_max=std::max<int32_t>(0,_page_height-viewport);
    const int32_t target=std::max<int32_t>(0,std::min(logical,logical_max));
    const int32_t physical=NomadNet::VirtualViewport::physical_from_logical(
        target,_page_height,viewport,_physical_extent);
    if(animation==LV_ANIM_OFF){
        if(!layout_window(target))return false;
        _logical_scroll=target;
    }
    lv_obj_scroll_to_y(_content,physical,animation);
    lv_obj_invalidate(_content);
    return true;
}

bool NomadNetScreen::jump_to_anchor(const std::string& name){
    if(!_page_loaded)return false;
    uint16_t block_index=0;
    int32_t target=-1;
    if(!name.empty()){
        if(!_page.find_anchor(name,block_index))return false;
        for(const auto& checkpoint:_layout_checkpoints){
            if(checkpoint.block_index==block_index){target=checkpoint.y;break;}
        }
    }else{
        for(const auto& checkpoint:_layout_checkpoints){
            if(checkpoint.y<=_logical_scroll||checkpoint.block_index>=_page.blocks().size())continue;
            if(_page.blocks()[checkpoint.block_index].type==NomadNet::BlockType::HEADING){
                target=checkpoint.y;break;
            }
        }
    }
    if(target<0)return false;
    scroll_to_logical(target,LV_ANIM_OFF);
    return true;
}

void NomadNetScreen::restore_logical_scroll(int32_t logical){
    if(_page_loaded)scroll_to_logical(logical,LV_ANIM_OFF);
}

void NomadNetScreen::draw_page(lv_event_t* event){
    auto* draw_ctx=lv_event_get_draw_ctx(event);
    lv_area_t content_area;
    lv_obj_get_content_coords(_content,&content_area);
    lv_area_t content_clip;
    if(!_lv_area_intersect(&content_clip,draw_ctx->clip_area,&content_area))return;
    const lv_area_t* original_clip=draw_ctx->clip_area;
    draw_ctx->clip_area=&content_clip;
    const int16_t top=content_area.y1;
    const int16_t left=content_area.x1;
    char scratch[256];
    for(const auto& fragment:_page_layout){
        const int32_t logical_y=_layout_window_top+fragment.y;
        const int32_t draw_y=top+logical_y-_logical_scroll;
        if(draw_y+fragment.height<_content->coords.y1||draw_y>_content->coords.y2)continue;
        lv_area_t area{static_cast<lv_coord_t>(left+fragment.x),static_cast<lv_coord_t>(draw_y),
            static_cast<lv_coord_t>(left+fragment.x+std::max<int16_t>(fragment.width,1)-1),
            static_cast<lv_coord_t>(draw_y+std::max<int16_t>(fragment.height,1)-1)};
        if(fragment.image_index>=0&&
           static_cast<std::size_t>(fragment.image_index)<_page.images().size()){
            uint16_t iw=0,ih=0;
            const uint16_t* pixels=decoded_image(fragment.image_index,iw,ih);
            if(pixels&&iw>0&&ih>0){
                // Decoded: blit the stored RGB565 buffer into the placement
                // rect (lv_draw_img_decoded skips src-type/decoder heuristics
                // entirely). LVGL 8.4 SW contract for the zoomed path: the
                // coords area's SIZE is read as the NATIVE source dimensions
                // (src_w/src_h, and the source stride), while its POSITION is
                // the screen anchor where the native image's top-left sits;
                // zoom/pivot scale it from there. (Using the display size as
                // coords size made LVGL treat the iw-wide buffer as disp_w
                // wide — a per-row stride drift that renders as a growing
                // diagonal offset.) The placement box always satisfies
                // width<=content_width and height<=maxrow (see
                // compute_image_placement), but a width-derived aspect fit
                // does NOT guarantee the intrinsic height matches the box:
                // a box constrained by the height bound (or a width/height
                // spec pair that does not preserve aspect) can be shorter
                // than the width-scaled image. So scale on BOTH axes
                // (uniform 8.8 zoom, no upscaling) so the result always fits
                // inside the box, and center the leftovers on either axis by
                // placing the native top-left anchor at the offset.
                const int32_t box_w=std::max<int32_t>(1,fragment.width);
                const int32_t box_h=std::max<int32_t>(1,fragment.height);
                const uint32_t zoom=std::max<uint32_t>(1u,std::min<uint32_t>(
                    (uint32_t)(box_w*256u/iw),
                    (uint32_t)(box_h*256u/ih)));
                // Display extent as LVGL's transform renders it (integer
                // division, same as the 8.8 scaling) so centering matches
                // the actual drawn size.
                const uint32_t disp_w=(iw*zoom)/256u;
                const uint32_t disp_h=(ih*zoom)/256u;
                const int32_t left_off=(box_w-(int32_t)disp_w)/2;
                const int32_t top_off=(box_h-(int32_t)disp_h)/2;
                lv_area_t decoded_area{static_cast<lv_coord_t>(area.x1+left_off),
                    static_cast<lv_coord_t>(area.y1+top_off),
                    static_cast<lv_coord_t>(area.x1+left_off+iw-1),
                    static_cast<lv_coord_t>(area.y1+top_off+ih-1)};
                lv_draw_img_dsc_t img_dsc;
                lv_draw_img_dsc_init(&img_dsc);
                img_dsc.zoom=zoom;
                lv_draw_img_decoded(draw_ctx,&img_dsc,&decoded_area,
                    reinterpret_cast<const uint8_t*>(pixels),
                    LV_IMG_CF_TRUE_COLOR);
            }else{
                // Placeholder: bounded surface box plus one body row of
                // alt-text (or the default notice) centered in the row.
                lv_draw_rect_dsc_t box;lv_draw_rect_dsc_init(&box);
                box.bg_color=Theme::surface();
                box.border_color=Theme::border();
                box.border_width=1;
                box.radius=3;
                lv_draw_rect(draw_ctx,&box,&area);
                const auto& image=_page.images()[fragment.image_index];
                const auto alt=_page.image_alt(image);
                const char* label=alt.size()!=0?
                    reinterpret_cast<const char*>(alt.data()):"[Image]";
                const std::size_t retained=safe_utf8_prefix(
                    label,alt.size()!=0?alt.size():7,sizeof(scratch)-1);
                if(retained!=0)std::memcpy(scratch,label,retained);
                scratch[retained]='\0';
                lv_draw_label_dsc_t dsc;lv_draw_label_dsc_init(&dsc);
                dsc.font=&nomadnet_font_12;dsc.color=Theme::border();
                const int32_t label_y=draw_y+(fragment.height-16)/2;
                lv_area_t text_area{area.x1+4,static_cast<lv_coord_t>(label_y),
                    static_cast<lv_coord_t>(std::max<int32_t>(area.x1,area.x2-4)),
                    static_cast<lv_coord_t>(label_y+15)};
                if(lv_area_get_height(&text_area)>0)
                    lv_draw_label(draw_ctx,&dsc,&text_area,scratch,nullptr);
            }
            continue;
        }
        if(fragment.divider){
            const std::size_t bytes=NomadNet::display_codepoint(fragment.divider_codepoint,scratch);
            const lv_font_t* font=&nomadnet_font_12;
            const int16_t glyph_width=static_cast<int16_t>(lv_txt_get_width(
                scratch,static_cast<uint32_t>(bytes),font,0,LV_TEXT_FLAG_NONE));
            if(glyph_width>0){
                lv_draw_label_dsc_t dsc;lv_draw_label_dsc_init(&dsc);
                dsc.font=font;dsc.color=Theme::border();dsc.letter_space=0;
                for(int16_t x=0;x<fragment.width;x=static_cast<int16_t>(x+glyph_width)){
                    lv_area_t glyph_area{static_cast<lv_coord_t>(area.x1+x),area.y1,
                        static_cast<lv_coord_t>(std::min<int32_t>(area.x1+x+glyph_width-1,area.x2)),area.y2};
                    lv_draw_label(draw_ctx,&dsc,&glyph_area,scratch,nullptr);
                }
            }
            continue;
        }
        if(fragment.table_cell){
            lv_draw_rect_dsc_t cell;lv_draw_rect_dsc_init(&cell);
            cell.bg_color=fragment.table_header?Theme::surfaceContainer():Theme::surface();
            cell.border_color=Theme::border();
            cell.border_width=1;
            lv_draw_rect(draw_ctx,&cell,&area);
            continue;
        }
        if(fragment.heading_starts_band()){
            lv_area_t band_area{content_area.x1,static_cast<lv_coord_t>(draw_y),content_area.x2,
                static_cast<lv_coord_t>(draw_y+std::max<int16_t>(fragment.height,1)-1)};
            lv_draw_rect_dsc_t band;lv_draw_rect_dsc_init(&band);
            band.bg_color=lv_color_hex(NomadNet::heading_background(fragment.heading_level()));
            lv_draw_rect(draw_ctx,&band,&band_area);
        }
        if(fragment.field_index>=0&&
           static_cast<std::size_t>(fragment.field_index)<_page.fields().size()&&
           static_cast<std::size_t>(fragment.field_index)<_form_state.fields().size()){
            const auto& field=_page.fields()[fragment.field_index];
            const auto& state=_form_state.fields()[fragment.field_index];
            lv_draw_rect_dsc_t box;lv_draw_rect_dsc_init(&box);
            box.bg_color=Theme::surfaceInput();box.radius=3;
            box.border_width=fragment.field_index==_selected_field?2:1;
            box.border_color=fragment.field_index==_selected_field?Theme::primary():Theme::border();
            lv_draw_rect(draw_ctx,&box,&area);
            std::size_t used=0;
            if(field.type==NomadNet::FormFieldType::CHECKBOX||field.type==NomadNet::FormFieldType::RADIO){
                const char* prefix=field.type==NomadNet::FormFieldType::RADIO?
                    (state.checked?"(*) ":"( ) "):(state.checked?"[x] ":"[ ] ");
                used=4;std::memcpy(scratch,prefix,used);
                const auto label=_page.field_label(fragment.field_index);
                const std::size_t retained=safe_utf8_prefix(
                    label.data(),label.size(),sizeof(scratch)-used-1);
                if(retained!=0)std::memcpy(scratch+used,label.data(),retained);
                used+=retained;
            }else{
                used=field.type==NomadNet::FormFieldType::PASSWORD?
                    std::min<std::size_t>(state.value_length,sizeof(scratch)-1):
                    safe_utf8_prefix(state.value.data(),state.value_length,sizeof(scratch)-1);
                if(field.type==NomadNet::FormFieldType::PASSWORD)
                    std::memset(scratch,'*',used);
                else if(used!=0)std::memcpy(scratch,state.value.data(),used);
            }
            scratch[used]='\0';
            lv_area_t text_area=area;text_area.x1=static_cast<lv_coord_t>(text_area.x1+4);
            text_area.y1=static_cast<lv_coord_t>(text_area.y1+2);
            lv_draw_label_dsc_t field_text;lv_draw_label_dsc_init(&field_text);
            field_text.font=&nomadnet_font_12;
            field_text.color=lv_color_hex(NomadNet::resolve_effective_foreground(
                _page,_page.runs()[fragment.run_index],fragment.heading_level(),Theme::TEXT_PRIMARY));
            lv_draw_label(draw_ctx,&field_text,&text_area,scratch,nullptr);
            continue;
        }
        if(fragment.run_index>=_page.runs().size())continue;
        const auto& run=_page.runs()[fragment.run_index];
        const auto text=_page.text(run);
        if(fragment.byte_offset+fragment.byte_length>text.size()||fragment.byte_length>=sizeof(scratch))continue;
        std::memcpy(scratch,text.data()+fragment.byte_offset,fragment.byte_length);scratch[fragment.byte_length]='\0';
        if(run.style&NomadNet::CompactPage::HAS_BACKGROUND){
            lv_draw_rect_dsc_t bg;lv_draw_rect_dsc_init(&bg);bg.bg_color=lv_color_hex(run.background);lv_draw_rect(draw_ctx,&bg,&area);
        }
        lv_draw_label_dsc_t dsc;lv_draw_label_dsc_init(&dsc);
        dsc.font=page_run_font(run, fragment.large_font);
        dsc.color=lv_color_hex(NomadNet::resolve_effective_foreground(
            _page,run,fragment.heading_level(),Theme::TEXT_PRIMARY));
        dsc.letter_space=0;
        dsc.decor=(run.style&NomadNet::CompactPage::UNDERLINE)||run.link_index>=0?LV_TEXT_DECOR_UNDERLINE:LV_TEXT_DECOR_NONE;
        lv_draw_label(draw_ctx,&dsc,&area,scratch,nullptr);
    }
    if(_selected_link>=0){
        NomadNet::for_each_focus_span(_page_layout,_selected_link,[&](const NomadNet::FocusSpan& span){
            if(span.run_index>=_page.runs().size())return;
            const int32_t draw_y=top+_layout_window_top+span.y-_logical_scroll;
            if(draw_y+span.height<_content->coords.y1||draw_y>_content->coords.y2)return;
            lv_area_t area{static_cast<lv_coord_t>(left+span.x),static_cast<lv_coord_t>(draw_y),
                static_cast<lv_coord_t>(left+span.x+std::max<int16_t>(span.width,1)-1),
                static_cast<lv_coord_t>(draw_y+std::max<int16_t>(span.height,1)-1)};
            lv_draw_rect_dsc_t focus;lv_draw_rect_dsc_init(&focus);focus.bg_opa=LV_OPA_TRANSP;
            focus.border_color=lv_color_hex(NomadNet::resolve_focus_border(
                _page,_page.runs()[span.run_index],Theme::SURFACE,span.heading_level));
            focus.border_width=1;lv_draw_rect(draw_ctx,&focus,&area);
        });
    }
    draw_ctx->clip_area=original_clip;
}

void NomadNetScreen::select_link(int direction){
    if(_focus_order.empty()){
        scroll_to_logical(_logical_scroll+direction*40,LV_ANIM_ON);
        return;
    }
    const int count=static_cast<int>(_focus_order.size());
    for(int attempt=0;attempt<count;++attempt){
        _selected_focus=_selected_focus<0?(direction>=0?0:count-1):
            static_cast<int16_t>((_selected_focus+direction+count)%count);
        const auto& choice=_focus_order[_selected_focus];
        if(choice.field)break;
        const int candidate=choice.index;
        if(candidate>=0&&static_cast<std::size_t>(candidate)<_link_y.size()&&_link_y[candidate]>=0)break;
    }
    const auto& selected=_focus_order[_selected_focus];
    _selected_link=selected.field?-1:static_cast<int16_t>(selected.index);
    _selected_field=selected.field?static_cast<int16_t>(selected.index):-1;
    const int32_t visible=lv_obj_get_content_height(_content);
    if(selected.y<_logical_scroll)scroll_to_logical(selected.y,LV_ANIM_ON);
    else if(selected.bottom>_logical_scroll+visible)
        scroll_to_logical(selected.bottom-visible,LV_ANIM_ON);
    lv_obj_invalidate(_content);
}

void NomadNetScreen::begin_field_edit(uint16_t field_id){
    if(field_id>=_form_state.fields().size()||field_id>=_page.fields().size())return;
    const auto& state=_form_state.fields()[field_id];
    const auto type=_page.fields()[field_id].type;
    if(type!=NomadNet::FormFieldType::TEXT&&type!=NomadNet::FormFieldType::PASSWORD)return;
    if(_field_editor)finish_field_edit(false);
    auto* previous_default_group=lv_group_get_default();
    lv_group_set_default(nullptr);
    _field_editor=lv_textarea_create(_screen);
    lv_group_set_default(previous_default_group);
    if(!_field_editor){
        _editing_field=-1;
        set_status("Field editor is unavailable");
        return;
    }
    auto discard_editor=[&](){
        if(!_field_editor)return;
        if(const char* value=lv_textarea_get_text(_field_editor)){
            volatile char* bytes=const_cast<char*>(value);
            const std::size_t wipe_length=std::strlen(value);
            for(std::size_t i=0;i<wipe_length;++i)bytes[i]=0;
        }
        if(type==NomadNet::FormFieldType::PASSWORD)
            lv_textarea_set_password_mode(_field_editor,false);
        lv_obj_del(_field_editor);
        _field_editor=nullptr;
        _editing_field=-1;
    };
    lv_obj_set_size(_field_editor,312,type==NomadNet::FormFieldType::PASSWORD?38:76);
    lv_obj_align(_field_editor,LV_ALIGN_BOTTOM_MID,0,-4);
    lv_textarea_set_one_line(_field_editor,type==NomadNet::FormFieldType::PASSWORD);
    lv_textarea_set_max_length(_field_editor,NomadNet::DocumentParser::MAX_FIELD_VALUE_BYTES);
    lv_obj_set_style_bg_color(_field_editor,Theme::surfaceInput(),0);
    if(!TextAreaHelper::enable_paste(_field_editor)||
       !lv_obj_add_event_cb(_field_editor,field_editor_event,
                            static_cast<lv_event_code_t>(LV_EVENT_ALL|LV_EVENT_PREPROCESS),this)){
        discard_editor();
        set_status("Field editor is unavailable");
        return;
    }
    _editing_field=static_cast<int16_t>(field_id);
    if(type==NomadNet::FormFieldType::PASSWORD){
        // Enable password mode while the editor is empty. The patched LVGL
        // constructor and setter leave the editor unchanged on allocation failure.
        lv_textarea_set_password_mode(_field_editor,true);
        if(type==NomadNet::FormFieldType::PASSWORD&&
       !lv_textarea_get_password_mode(_field_editor)){
            discard_editor();
            set_status("Field editor is unavailable");
            return;
        }
        if(lv_textarea_get_text(_field_editor)==nullptr){
            discard_editor();
            set_status("Field editor is unavailable");
            return;
        }
    }
    lv_textarea_set_text(_field_editor,state.value.data());
    const char* loaded_value=lv_textarea_get_text(_field_editor);
    if(!loaded_value||std::strcmp(loaded_value,state.value.data())!=0){
        discard_editor();
        set_status("Field editor is unavailable");
        return;
    }
    lv_obj_clear_flag(_field_editor,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_field_editor);
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        lv_group_add_obj(group,_field_editor);
        if(lv_obj_get_group(_field_editor)!=group){
            discard_editor();
            set_status("Field editor is unavailable");
            return;
        }
        lv_group_remove_obj(_content);
        lv_group_focus_obj(_field_editor);
        lv_group_set_editing(group,true);
    }
}

void NomadNetScreen::finish_field_edit(bool accept){
    if(!_field_editor)return;
    if(_editing_field>=0&&accept){
        const char* value=lv_textarea_get_text(_field_editor);
        const std::size_t length=value?std::strlen(value):0;
        if(!_form_state.set_value(static_cast<uint16_t>(_editing_field),value,length))
            set_status("Field value exceeds device limit");
    }
    if(const char* value=lv_textarea_get_text(_field_editor)){
        volatile char* bytes=const_cast<char*>(value);
        const std::size_t length=std::strlen(value);
        for(std::size_t i=0;i<length;++i)bytes[i]=0;
    }
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group)lv_group_remove_obj(_field_editor);
    lv_textarea_set_password_mode(_field_editor,false);
    lv_textarea_set_text(_field_editor,"");
    lv_obj_del(_field_editor);
    _field_editor=nullptr;
    _editing_field=-1;
    if(_visible&&_view==View::BROWSER&&group){
        lv_group_add_obj(group,_content);
        lv_group_focus_obj(_content);
        lv_group_set_editing(group,true);
    }
    if(_content)lv_obj_invalidate(_content);
}

void NomadNetScreen::activate_selected_link(){
    if(_selected_field>=0&&static_cast<std::size_t>(_selected_field)<_page.fields().size()){
        const auto type=_page.fields()[_selected_field].type;
        if(type==NomadNet::FormFieldType::CHECKBOX){
            const bool checked=!_form_state.fields()[_selected_field].checked;
            _form_state.set_checked(static_cast<uint16_t>(_selected_field),checked);
            lv_obj_invalidate(_content);
        }else if(type==NomadNet::FormFieldType::RADIO){
            _form_state.set_checked(static_cast<uint16_t>(_selected_field),true);
            lv_obj_invalidate(_content);
        }else begin_field_edit(static_cast<uint16_t>(_selected_field));
        return;
    }
    if(_selected_link<0||static_cast<std::size_t>(_selected_link)>=_page.links().size())return;
    const auto target_view=_page.target(static_cast<std::size_t>(_selected_link));
    const std::string target(target_view.data(),target_view.size());
    const bool submitted=target.find('`')!=std::string::npos;
    if(submitted){
        if(!_submit||!_submit(static_cast<uint16_t>(_selected_link),_form_generation))
            set_status("Browser action queue is busy");
    }else if(!_link||!_link(target))set_status("Browser action queue is busy");
}

bool NomadNetScreen::prepare_submission(uint16_t link_id,uint32_t generation,
                                        std::string& target,
                                        NomadNet::ExternalVector<uint8_t>& request_data,
                                        NomadNet::FormEncodeResult& result)const{
    target.clear();NomadNet::clear_encoded_form(request_data);
    result=NomadNet::FormEncodeResult::INVALID_STATE;
    if(generation!=_form_generation||link_id>=_page.links().size())return false;
    const auto view=_page.target(link_id);
    if(!view.data())return false;
    try{
        target.assign(view.data(),view.size());
        const std::size_t separator=target.find('`');
        if(separator==std::string::npos)return false;
        result=_form_state.encode(target.substr(separator+1),request_data);
        return result==NomadNet::FormEncodeResult::OK;
    }catch(const std::bad_alloc&){
        if(!target.empty()){
            volatile char* bytes=&target[0];
            for(std::size_t i=0;i<target.size();++i)bytes[i]=0;
        }
        std::string().swap(target);
        NomadNet::clear_encoded_form(request_data);
        result=NomadNet::FormEncodeResult::ALLOCATION_FAILED;
        return false;
    }
}

void NomadNetScreen::field_editor_event(lv_event_t* event){
    auto* self=static_cast<NomadNetScreen*>(lv_event_get_user_data(event));
    auto finish_editor=[&](bool accept){
        auto* editor=self->_field_editor;
        auto* indev=lv_indev_get_act();
        lv_event_stop_processing(event);
        if(indev&&editor)lv_indev_reset(indev,editor);
        self->finish_field_edit(accept);
    };
    const auto code=lv_event_get_code(event);
    if(code==LV_EVENT_READY)finish_editor(true);
    else if(code==LV_EVENT_CANCEL)finish_editor(false);
    else if(code==LV_EVENT_KEY){
        const uint32_t key=lv_event_get_key(event);
        auto* indev=lv_indev_get_act();
        if(key==LV_KEY_ENTER&&indev==LVGL::LVGLInit::get_keyboard()){
            finish_editor(true);
        }else if(key==LV_KEY_ENTER&&indev==LVGL::LVGLInit::get_trackball()){
            auto* editor=self->_field_editor;
            if(editor){
                if(lv_textarea_get_one_line(editor))finish_editor(true);
                else{
                    lv_event_stop_processing(event);
                    lv_textarea_add_char(editor,'\n');
                }
            }
        }else if(key==LV_KEY_ESC)finish_editor(false);
    }
}

void NomadNetScreen::page_event(lv_event_t* event){
    auto* self=static_cast<NomadNetScreen*>(lv_event_get_user_data(event));
    const auto code=lv_event_get_code(event);
    if(code==LV_EVENT_DRAW_MAIN)self->draw_page(event);
    else if(code==LV_EVENT_GET_SELF_SIZE){auto* size=static_cast<lv_point_t*>(lv_event_get_param(event));size->y=std::max<lv_coord_t>(size->y,static_cast<lv_coord_t>(self->_physical_extent));}
    else if(code==LV_EVENT_SCROLL){
        if(self->_transaction_scroll_restore){
            lv_obj_invalidate(self->_content);
            return;
        }
        self->_logical_scroll=self->logical_scroll_from_widget();
        if(!self->layout_window(self->_logical_scroll))self->set_status("Page viewport could not be retained");
        lv_obj_invalidate(self->_content);
    }
    else if(code==LV_EVENT_FOCUSED){
        auto* group=static_cast<lv_group_t*>(lv_obj_get_group(self->_content));
        if(group)lv_group_set_editing(group,true);
        if(self->_selected_focus<0&&!self->_focus_order.empty())self->select_link(1);
    }
    else if(code==LV_EVENT_DEFOCUSED){
        auto* group=static_cast<lv_group_t*>(lv_obj_get_group(self->_content));
        if(group&&lv_group_get_editing(group))lv_group_set_editing(group,false);
    }
    else if(code==LV_EVENT_KEY){
        const uint32_t key=lv_event_get_key(event);
        if(key==LV_KEY_DOWN||key==LV_KEY_RIGHT)self->select_link(1);
        else if(key==LV_KEY_UP)self->select_link(-1);
        else if(key==LV_KEY_LEFT){
            auto* group=static_cast<lv_group_t*>(lv_obj_get_group(self->_content));
            if(group){lv_group_set_editing(group,false);lv_group_focus_obj(self->_edit_button);}
        }else if(key==LV_KEY_ENTER)self->activate_selected_link();
    }
    else if(code==LV_EVENT_CLICKED){
        auto* indev=lv_indev_get_act();
        if(!indev||lv_indev_get_type(indev)!=LV_INDEV_TYPE_POINTER)return;
        lv_point_t point;lv_indev_get_point(indev,&point);
        lv_area_t content_area;
        lv_obj_get_content_coords(self->_content,&content_area);
        if(!_lv_area_is_point_on(&content_area,&point,0))return;
        const int x=point.x-content_area.x1;
        const int32_t y=point.y-content_area.y1+self->_logical_scroll;
        for(const auto& fragment:self->_page_layout){
            const int32_t fragment_y=self->_layout_window_top+fragment.y;
            if(x<fragment.x||x>=fragment.x+fragment.width||
               y<fragment_y||y>=fragment_y+fragment.height)continue;
            if(fragment.field_index>=0){
                self->_selected_field=fragment.field_index;self->_selected_link=-1;
                for(std::size_t i=0;i<self->_focus_order.size();++i)
                    if(self->_focus_order[i].field&&self->_focus_order[i].index==fragment.field_index)
                        self->_selected_focus=static_cast<int16_t>(i);
                self->activate_selected_link();break;
            }
            if(fragment.link_index>=0){
                self->_selected_link=fragment.link_index;self->_selected_field=-1;
                for(std::size_t i=0;i<self->_focus_order.size();++i)
                    if(!self->_focus_order[i].field&&self->_focus_order[i].index==fragment.link_index)
                        self->_selected_focus=static_cast<int16_t>(i);
                self->activate_selected_link();break;
            }
        }
    }
}
void NomadNetScreen::show(){
    _visible=true;lv_obj_clear_flag(_screen,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(_screen);
    rebuild_focus();
}
void NomadNetScreen::hide(){
    finish_field_edit(false);
    _visible=false;auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        if(lv_group_get_editing(group)&&lv_group_get_focused(group)==_content)lv_group_set_editing(group,false);
        for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_identify_button,_address,_go_button,_edit_button})lv_group_remove_obj(object);
        lv_group_remove_obj(_content);
        for(auto* object:_directory_focusables)lv_group_remove_obj(object);
    }
    lv_obj_add_flag(_screen,LV_OBJ_FLAG_HIDDEN);
}
void NomadNetScreen::clicked(lv_event_t* event){
    auto* self=static_cast<NomadNetScreen*>(lv_event_get_user_data(event));auto* target=lv_event_get_target(event);
    if(target==self->_back_button&&self->_back)self->_back();
    else if(target==self->_home_button&&self->_home)self->_home();
    else if(target==self->_reload_button&&self->_reload){const std::string address=self->address();if(!self->_reload(address))self->set_status("Browser action queue is busy");}
    else if(target==self->_load_images_button&&self->_load_images){const std::string address=self->address();if(!self->_load_images(address))self->set_status("Browser action queue is busy");}
    else if(target==self->_save_button&&self->_save){if(!self->_save(self->address()))self->set_status("Browser action queue is busy");}
    else if(target==self->_identify_button&&self->_identify){if(!self->_identify(self->address(),!self->_identify_enabled))self->set_status("Browser action queue is busy");}
    else if(target==self->_edit_button){self->set_address_editing(true);self->set_status("Edit destination or page path");}
    else if((target==self->_go_button||target==self->_address)&&self->_open){const std::string address=self->address();if(!self->_open(address))self->set_status("Browser action queue is busy");}
    else{
        const std::size_t code=reinterpret_cast<std::size_t>(lv_obj_get_user_data(target));
        if(code==1001)self->render_directory(View::HEARD);
        else if(code==1002)self->render_directory(View::SAVED_NODES);
        else if(code==1003)self->render_directory(View::SAVED_PAGES);
        else if(code==1004)self->render_directory(View::RECENT);
        else if(code==1005){self->clear_document();self->show_browser(true);self->set_status("Enter a NomadNet address");}
        else if(code>2000&&code<=2000+self->_directory_targets.size()){
            const std::string selected=self->_directory_targets[code-2001];
            if(!self->_open||!self->_open(selected))self->set_status("Browser action queue is busy");
        }
    }
}
}
#endif
