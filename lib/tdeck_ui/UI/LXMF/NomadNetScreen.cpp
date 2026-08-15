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
    for(auto* button:{_back_button,_home_button,_reload_button,_save_button}) {
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
    for(auto* o:{_back_button,_home_button,_reload_button,_save_button,_go_button,_edit_button})lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,this);
    lv_obj_add_event_cb(_address,clicked,LV_EVENT_READY,this);
    lv_obj_add_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
    set_status("Enter a NomadNet address");show_start();hide();
}
NomadNetScreen::~NomadNetScreen(){if(_screen)lv_obj_del(_screen);}
void NomadNetScreen::set_address(const std::string& value){
    lv_textarea_set_text(_address,value.c_str());
    const auto summary=NomadNet::display_text(NomadNet::compact_address(value));
    lv_label_set_text(_address_summary,summary.c_str());
}
std::string NomadNetScreen::address()const{return lv_textarea_get_text(_address);}
void NomadNetScreen::set_library(const NomadNet::Library& library){
    _library=library;
    if(_view!=View::BROWSER)render_directory(_view);
}
void NomadNetScreen::set_page_saved(bool saved){
    lv_obj_set_style_bg_color(_save_button,saved?Theme::primary():Theme::surfaceContainer(),0);
}
void NomadNetScreen::clear_document(){
    nomad_screen_heap_checkpoint("clear-before");
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        if(lv_group_get_editing(group)&&lv_group_get_focused(group)==_content)lv_group_set_editing(group,false);
        lv_group_remove_obj(_content);
    }
    _page.clear();
    NomadNet::ExternalVector<LayoutFragment>().swap(_page_layout);
    _page_height=0;
    _selected_link=-1;
    lv_obj_refresh_self_size(_content);
    lv_obj_invalidate(_content);
    _page_loaded=false;
    set_page_saved(false);
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
    if(_page_loaded)lv_obj_clear_flag(_save_button,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(_save_button,LV_OBJ_FLAG_HIDDEN);
    set_address_editing(editing);
    rebuild_focus();
}
void NomadNetScreen::render_directory(View view){
    clear_directory();
    _view=view;
    _directory_visible.store(true,std::memory_order_release);
    lv_obj_clear_flag(_directory,LV_OBJ_FLAG_HIDDEN);
    for(auto* object:{_address_row,_status,_content,_reload_button,_save_button})lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);

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
        for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_address,_go_button,_edit_button})
            if(candidate==object)return true;
        if(candidate==_content)return true;
        for(auto* object:_directory_focusables)if(candidate==object)return true;
        return false;
    };
    auto detach_non_owner=[&](lv_obj_t* object){
        if(object&&object!=focused)lv_group_remove_obj(object);
    };
    for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_address,_go_button,_edit_button})detach_non_owner(object);
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
void NomadNetScreen::set_status(const char* value){
    const std::string status=value?value:"";
    lv_label_set_text(_status,status.c_str());
    const bool loaded_ack=_page_loaded&&status.rfind("Page loaded",0)==0;
    apply_browser_layout(!loaded_ack);
}
bool NomadNetScreen::set_page(const NomadNet::Document& document) {
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group)lv_group_remove_obj(_content);
    if(!_page.assign(document)) {
        clear_document();
        set_status("Page is too large for available memory");
        return false;
    }
    if(_page.truncated()){
        const std::string notice=NomadNet::truncation_notice(document);
        if(!_page.append_notice(notice)){
            clear_document();
            set_status("Page truncation notice could not be retained");
            return false;
        }
    }
    else if(_page.unsupported())_page.append_notice("[Unsupported Micron content]");
    if(!layout_page()){
        clear_document();
        set_status("Page layout notice could not be retained");
        return false;
    }
    lv_obj_set_style_bg_color(_content,_page.has_background()
        ?lv_color_hex(_page.background()):Theme::surface(),0);
    _page_loaded=true;
    show_browser(false);
    lv_obj_scroll_to_y(_content,0,LV_ANIM_OFF);
    lv_obj_refresh_self_size(_content);
    lv_obj_invalidate(_content);
    return true;
}

bool NomadNetScreen::layout_page(){
    _page_layout.clear();
    _page_layout.reserve(std::min<std::size_t>(_page.runs().size()*4,MAX_LAYOUT_FRAGMENTS));
    constexpr std::size_t content_fragment_limit=MAX_LAYOUT_FRAGMENTS-1;
    constexpr int16_t width=304;
    int16_t y=0;
    bool content_remaining=false;
    for(std::size_t block_index=0;block_index<_page.blocks().size();++block_index){
        if(_page_layout.size()>=content_fragment_limit){
            content_remaining=std::any_of(_page.blocks().begin()+block_index,_page.blocks().end(),
                [](const NomadNet::CompactPage::BlockRecord& candidate){
                    return NomadNet::block_has_layout_content(candidate.type,candidate.run_count);
                });
            break;
        }
        const auto& block=_page.blocks()[block_index];
        if(block.type==NomadNet::BlockType::DIVIDER){
            _page_layout.push_back(LayoutFragment(0,0,0,-1,0,y,width,1,true));y+=9;continue;
        }
        if(block.run_count==0)continue;
        const int16_t indent=block.type==NomadNet::BlockType::HEADING?0:
            static_cast<int16_t>(std::min<unsigned>(block.depth,8)*4);
        const int16_t available=width-indent;
        int16_t x=indent;
        int16_t line_h=16;
        std::size_t line_first=_page_layout.size();
        int16_t line_y=y;
        auto align_line=[&](std::size_t begin,std::size_t end,int16_t current_y){
            if(begin>=end||block.alignment==NomadNet::Alignment::LEFT)return;
            int16_t line_width=0;
            for(std::size_t i=begin;i<end;++i)if(_page_layout[i].y==current_y)
                line_width=std::max<int16_t>(line_width,_page_layout[i].x+_page_layout[i].width-indent);
            const int16_t shift=block.alignment==NomadNet::Alignment::CENTER
                ?(available-line_width)/2:available-line_width;
            if(shift>0)for(std::size_t i=begin;i<end;++i)
                if(_page_layout[i].y==current_y)_page_layout[i].x+=shift;
        };
        for(uint16_t r=0;r<block.run_count&&block.first_run+r<_page.runs().size();++r){
            if(_page_layout.size()>=content_fragment_limit){content_remaining=true;break;}
            const uint16_t run_index=static_cast<uint16_t>(block.first_run+r);
            const auto& run=_page.runs()[run_index];
            const auto text=_page.text(run);
            const bool large=block.type==NomadNet::BlockType::HEADING;
            const lv_font_t* font=page_run_font(run,large);
            const int16_t height=static_cast<int16_t>(font->line_height+3);
            line_h=std::max(line_h,height);
            std::size_t offset=0;
            while(offset<text.size()){
                if(_page_layout.size()>=content_fragment_limit){content_remaining=true;break;}
                std::size_t end=offset;
                const bool whitespace=text[offset]==' '||text[offset]=='\t';
                while(end<text.size()&&((text[end]==' '||text[end]=='\t')==whitespace)&&end-offset<255)++end;
                if(end==offset)++end;
                int16_t fragment_w=static_cast<int16_t>(lv_txt_get_width(text.data()+offset,
                    static_cast<uint32_t>(end-offset),font,0,LV_TEXT_FLAG_NONE));
                if(whitespace&&x+fragment_w>indent+available){offset=end;continue;}
                if(!whitespace&&x>indent&&x+fragment_w>indent+available){
                    align_line(line_first,_page_layout.size(),line_y);
                    y+=line_h;x=indent;line_h=height;line_first=_page_layout.size();line_y=y;
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
                if(!(whitespace&&x==indent)){
                    _page_layout.push_back(LayoutFragment(run_index,static_cast<uint16_t>(offset),
                        static_cast<uint16_t>(end-offset),run.link_index,x,y,fragment_w,height,false,
                        large));
                    x=static_cast<int16_t>(x+fragment_w);
                }
                offset=end;
            }
            if(content_remaining)break;
        }
        align_line(line_first,_page_layout.size(),line_y);
        y+=line_h+3;
        if(content_remaining)break;
    }
    if(NomadNet::layout_content_truncated(_page_layout.size(),content_fragment_limit,
                                         content_remaining)){
        const std::string layout_notice="[Page layout truncated: "+
            std::to_string(content_fragment_limit)+" fragments]";
        if(!_page.append_notice(layout_notice))return false;
        const uint16_t notice_run_index=static_cast<uint16_t>(_page.runs().size()-1);
        _page_layout.erase(std::remove_if(_page_layout.begin(),_page_layout.end(),
            [notice_run_index](const LayoutFragment& fragment){
                return !fragment.divider&&fragment.run_index>=notice_run_index;
            }),_page_layout.end());
        const auto notice=_page.text(_page.runs()[notice_run_index]);
        const int16_t notice_width=static_cast<int16_t>(std::min<lv_coord_t>(304,
            lv_txt_get_width(notice.data(),static_cast<uint32_t>(notice.size()),
                &nomadnet_font_12,0,LV_TEXT_FLAG_NONE)));
        _page_layout.push_back(LayoutFragment(notice_run_index,0,
            static_cast<uint16_t>(notice.size()),-1,0,y,notice_width,19,false));
        y+=22;
    }
    _page_height=std::max<int32_t>(y,lv_obj_get_content_height(_content));
    return true;
}

void NomadNetScreen::draw_page(lv_event_t* event){
    auto* draw_ctx=lv_event_get_draw_ctx(event);
    const int16_t scroll=lv_obj_get_scroll_y(_content);
    const int16_t top=_content->coords.y1+lv_obj_get_style_pad_top(_content,0);
    const int16_t left=_content->coords.x1+lv_obj_get_style_pad_left(_content,0);
    char scratch[256];
    for(const auto& fragment:_page_layout){
        const int16_t draw_y=static_cast<int16_t>(top+fragment.y-scroll);
        if(draw_y+fragment.height<_content->coords.y1||draw_y>_content->coords.y2)continue;
        lv_area_t area{static_cast<lv_coord_t>(left+fragment.x),draw_y,
            static_cast<lv_coord_t>(left+fragment.x+std::max<int16_t>(fragment.width,1)-1),
            static_cast<lv_coord_t>(draw_y+std::max<int16_t>(fragment.height,1)-1)};
        if(fragment.divider){lv_draw_rect_dsc_t dsc;lv_draw_rect_dsc_init(&dsc);dsc.bg_color=Theme::border();lv_draw_rect(draw_ctx,&dsc,&area);continue;}
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
        dsc.color=lv_color_hex(NomadNet::resolve_foreground(_page,run,Theme::TEXT_PRIMARY));
        dsc.letter_space=0;
        dsc.decor=(run.style&NomadNet::CompactPage::UNDERLINE)||run.link_index>=0?LV_TEXT_DECOR_UNDERLINE:LV_TEXT_DECOR_NONE;
        lv_draw_label(draw_ctx,&dsc,&area,scratch,nullptr);
    }
    if(_selected_link>=0){
        NomadNet::for_each_focus_span(_page_layout,_selected_link,[&](const NomadNet::FocusSpan& span){
            if(span.run_index>=_page.runs().size())return;
            const int16_t draw_y=static_cast<int16_t>(top+span.y-scroll);
            if(draw_y+span.height<_content->coords.y1||draw_y>_content->coords.y2)return;
            lv_area_t area{static_cast<lv_coord_t>(left+span.x),draw_y,
                static_cast<lv_coord_t>(left+span.x+std::max<int16_t>(span.width,1)-1),
                static_cast<lv_coord_t>(draw_y+std::max<int16_t>(span.height,1)-1)};
            lv_draw_rect_dsc_t focus;lv_draw_rect_dsc_init(&focus);focus.bg_opa=LV_OPA_TRANSP;
            focus.border_color=lv_color_hex(NomadNet::resolve_focus_border(
                _page,_page.runs()[span.run_index],Theme::SURFACE));
            focus.border_width=1;lv_draw_rect(draw_ctx,&focus,&area);
        });
    }
}

void NomadNetScreen::select_link(int direction){
    if(_page.links().empty()){
        const int next=std::max(0,lv_obj_get_scroll_y(_content)+direction*40);
        lv_obj_scroll_to_y(_content,next,LV_ANIM_ON);
        return;
    }
    const int count=static_cast<int>(_page.links().size());
    int candidate=_selected_link;
    for(int attempt=0;attempt<count;++attempt){
        candidate=candidate<0?(direction>=0?0:count-1):(candidate+direction+count)%count;
        bool rendered=false;
        for(const auto& fragment:_page_layout)if(fragment.link_index==candidate){rendered=true;break;}
        if(rendered){_selected_link=candidate;break;}
    }
    if(_selected_link<0)return;
    for(const auto& fragment:_page_layout)if(fragment.link_index==_selected_link){
        const int top=fragment.y;const int bottom=fragment.y+fragment.height;
        const int scroll=lv_obj_get_scroll_y(_content);const int visible=lv_obj_get_content_height(_content);
        if(top<scroll)lv_obj_scroll_to_y(_content,top,LV_ANIM_ON);
        else if(bottom>scroll+visible)lv_obj_scroll_to_y(_content,bottom-visible,LV_ANIM_ON);
        break;
    }
    lv_obj_invalidate(_content);
}

void NomadNetScreen::activate_selected_link(){
    if(_selected_link<0||static_cast<std::size_t>(_selected_link)>=_page.links().size()||!_link)return;
    const auto target_view=_page.target(static_cast<std::size_t>(_selected_link));
    const std::string target(target_view.data(),target_view.size());
    if(!_link(target))set_status("Browser action queue is busy");
}

void NomadNetScreen::page_event(lv_event_t* event){
    auto* self=static_cast<NomadNetScreen*>(lv_event_get_user_data(event));
    const auto code=lv_event_get_code(event);
    if(code==LV_EVENT_DRAW_MAIN)self->draw_page(event);
    else if(code==LV_EVENT_GET_SELF_SIZE){auto* size=static_cast<lv_point_t*>(lv_event_get_param(event));size->y=std::max<lv_coord_t>(size->y,self->_page_height);}
    else if(code==LV_EVENT_FOCUSED){
        auto* group=static_cast<lv_group_t*>(lv_obj_get_group(self->_content));
        if(group)lv_group_set_editing(group,true);
        if(self->_selected_link<0&&!self->_page.links().empty())self->select_link(1);
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
        lv_point_t point;lv_indev_get_point(indev,&point);const int scroll=lv_obj_get_scroll_y(self->_content);
        const int x=point.x-self->_content->coords.x1-lv_obj_get_style_pad_left(self->_content,0);
        const int y=point.y-self->_content->coords.y1-lv_obj_get_style_pad_top(self->_content,0)+scroll;
        for(const auto& fragment:self->_page_layout)if(fragment.link_index>=0&&x>=fragment.x&&x<fragment.x+fragment.width&&y>=fragment.y&&y<fragment.y+fragment.height){self->_selected_link=fragment.link_index;self->activate_selected_link();break;}
    }
}
void NomadNetScreen::show(){
    _visible=true;lv_obj_clear_flag(_screen,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(_screen);
    rebuild_focus();
}
void NomadNetScreen::hide(){
    _visible=false;auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        if(lv_group_get_editing(group)&&lv_group_get_focused(group)==_content)lv_group_set_editing(group,false);
        for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_address,_go_button,_edit_button})lv_group_remove_obj(object);
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
    else if(target==self->_save_button&&self->_save){if(!self->_save(self->address()))self->set_status("Browser action queue is busy");}
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
