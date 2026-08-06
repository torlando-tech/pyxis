#include "NomadNetScreen.h"
#ifdef ARDUINO
#include "Theme.h"
#include "NomadNetDisplay.h"
#include "NomadNetGlyphs.h"
#include "../LVGL/LVGLInit.h"
#include "../TextAreaHelper.h"
#include <algorithm>
#include <cstdio>

namespace UI::LXMF {
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
    lv_obj_set_style_text_font(_address,&nomadnet_font_12,0);lv_obj_set_style_bg_color(_address,Theme::surfaceInput(),0);TextAreaHelper::enable_paste(_address);
    _go_button=lv_btn_create(_address_row);lv_obj_set_size(_go_button,42,30);lv_obj_align(_go_button,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_set_style_bg_color(_go_button,Theme::primary(),0);lv_obj_set_style_radius(_go_button,8,0);
    lv_obj_t* gl=lv_label_create(_go_button);lv_label_set_text(gl,"Go");lv_obj_center(gl);

    _address_summary=lv_label_create(_address_row);lv_obj_set_size(_address_summary,266,24);lv_obj_align(_address_summary,LV_ALIGN_LEFT_MID,4,0);
    lv_label_set_long_mode(_address_summary,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(_address_summary,&nomadnet_font_12,0);
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
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group)for(auto* object:_focusables)lv_group_remove_obj(object);
    _focusables.clear();
    _link_targets.clear();
    lv_obj_clean(_content);
    _page_loaded=false;
    set_page_saved(false);
}
void NomadNetScreen::begin_navigation(const std::string& target){
    clear_document();
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
    auto* group=LVGL::LVGLInit::get_default_group();
    if(group)detach_focusables(group);
    _directory_focusables.clear();_directory_targets.clear();lv_obj_clean(_directory);_view=view;
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
        lv_obj_set_width(primary,title_width);lv_obj_set_style_text_font(primary,&nomadnet_font_12,0);lv_obj_align(primary,LV_ALIGN_TOP_LEFT,title_x,0);
        if(!detail.empty()){
            const auto rendered_detail=NomadNet::display_text(detail);
            lv_obj_t* secondary=lv_label_create(button);lv_label_set_text(secondary,rendered_detail.c_str());lv_label_set_long_mode(secondary,LV_LABEL_LONG_DOT);
            lv_obj_set_width(secondary,286);lv_obj_set_style_text_font(secondary,&nomadnet_font_12,0);lv_obj_set_style_text_color(secondary,Theme::textTertiary(),0);
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
        lv_obj_set_style_text_color(empty,Theme::textTertiary(),0);lv_obj_set_style_text_font(empty,&nomadnet_font_12,0);
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
        for(auto* object:_focusables)if(candidate==object)return true;
        for(auto* object:_directory_focusables)if(candidate==object)return true;
        return false;
    };
    auto detach_non_owner=[&](lv_obj_t* object){
        if(object&&object!=focused)lv_group_remove_obj(object);
    };
    for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_address,_go_button,_edit_button})detach_non_owner(object);
    for(auto* object:_focusables)detach_non_owner(object);
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
    for(auto* object:_focusables)lv_group_add_obj(group,object);
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
void NomadNetScreen::set_page(const NomadNet::Document& document) {
    auto* group = LVGL::LVGLInit::get_default_group();
    if (group) for (auto* object : _focusables) lv_group_remove_obj(object);
    lv_obj_clean(_content);
    _focusables.clear();
    _link_targets.clear();
    std::size_t objects = 0;
    std::size_t spans = 0;
    bool render_truncated = false;
    lv_obj_set_style_bg_color(_content, document.has_background
        ? lv_color_hex(document.background) : Theme::surface(), 0);

    for (const auto& block : document.blocks) {
        if (objects >= MAX_UI_OBJECTS || spans >= MAX_UI_SPANS) {
            render_truncated = true;
            break;
        }
        if (block.type == NomadNet::BlockType::DIVIDER) {
            lv_obj_t* line = lv_obj_create(_content);
            lv_obj_set_size(line, 304, 1);
            lv_obj_set_style_bg_color(line, Theme::border(), 0);
            lv_obj_set_style_border_width(line, 0, 0);
            ++objects;
            continue;
        }

        // Spans preserve mixed inline styling and wrap the complete source line
        // together instead of introducing breaks at every style transition.
        lv_obj_t* text = lv_spangroup_create(_content);
        const lv_coord_t indent = block.type == NomadNet::BlockType::HEADING
            ? 0 : static_cast<lv_coord_t>(std::min<unsigned>(block.depth, 8) * 4);
        lv_obj_set_width(text, 304 - indent);
        lv_obj_set_style_translate_x(text, indent, 0);
        lv_spangroup_set_mode(text, LV_SPAN_MODE_BREAK);
        lv_spangroup_set_overflow(text, LV_SPAN_OVERFLOW_CLIP);
        lv_spangroup_set_lines(text, -1);
        lv_spangroup_set_align(text, block.alignment == NomadNet::Alignment::CENTER
            ? LV_TEXT_ALIGN_CENTER : block.alignment == NomadNet::Alignment::RIGHT
                ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
        ++objects;

        for (const auto& run : block.runs) {
            if (spans >= MAX_UI_SPANS) { render_truncated = true; break; }
            lv_span_t* span = lv_spangroup_new_span(text);
            const auto rendered_text = NomadNet::display_text(run.text);
            lv_span_set_text(span, rendered_text.c_str());
            const bool is_link = run.link_index >= 0 &&
                static_cast<std::size_t>(run.link_index) < document.links.size();
            lv_style_set_text_color(&span->style, is_link ? Theme::primaryLight() :
                run.has_foreground ? lv_color_hex(run.foreground) :
                document.has_foreground ? lv_color_hex(document.foreground) : Theme::textPrimary());
            lv_style_set_text_font(&span->style,
                (run.bold || block.type == NomadNet::BlockType::HEADING)
                    ? &nomadnet_font_16 : &nomadnet_font_12);
            lv_style_set_text_line_space(&span->style, 3);
            if (run.underline || is_link)
                lv_style_set_text_decor(&span->style, LV_TEXT_DECOR_UNDERLINE);
            if (run.italic) lv_style_set_text_letter_space(&span->style, 1);
            if (run.has_background) lv_style_set_bg_color(&span->style, lv_color_hex(run.background));
            ++spans;
        }
        lv_spangroup_refr_mode(text);

        // A visual span cannot itself participate in the encoder focus group.
        // Add one bounded, focusable activation control for every rendered link.
        for (const auto& run : block.runs) {
            if (run.link_index < 0 || static_cast<std::size_t>(run.link_index) >= document.links.size()) continue;
            if (objects + 2 > MAX_UI_OBJECTS) { render_truncated = true; break; }
            lv_obj_t* button = lv_btn_create(_content);
            lv_obj_set_size(button, 304, 28);
            lv_obj_set_style_pad_all(button, 4, 0);
            lv_obj_t* label = lv_label_create(button);
            const std::string caption = "Open: " + NomadNet::display_text(run.text);
            lv_label_set_text(label, caption.c_str());
            lv_obj_set_style_text_font(label, &nomadnet_font_12, 0);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(label, 294);
            lv_obj_center(label);
            _link_targets.push_back(document.links[run.link_index].target);
            lv_obj_set_user_data(button, reinterpret_cast<void*>(_link_targets.size()));
            lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, this);
            _focusables.push_back(button);
            if (_visible && _view == View::BROWSER && group) lv_group_add_obj(group, button);
            objects += 2;
        }
    }
    if (document.truncated || render_truncated) {
        lv_obj_t* notice = lv_label_create(_content);
        lv_label_set_text(notice, "[Page truncated to device safety limits]");
        lv_obj_set_style_text_color(notice, Theme::warning(), 0);
    } else if (document.unsupported) {
        lv_obj_t* notice = lv_label_create(_content);
        lv_label_set_text(notice, "Forms, partials, tables and downloads are unsupported in this MVP.");
        lv_obj_set_style_text_color(notice, Theme::warning(), 0);
    }
    _page_loaded=true;
    show_browser(false);
    lv_obj_scroll_to_y(_content,0,LV_ANIM_OFF);
}
void NomadNetScreen::show(){
    _visible=true;lv_obj_clear_flag(_screen,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(_screen);
    rebuild_focus();
}
void NomadNetScreen::hide(){
    _visible=false;auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        for(auto* object:{_back_button,_home_button,_reload_button,_save_button,_address,_go_button,_edit_button})lv_group_remove_obj(object);
        for(auto* object:_focusables)lv_group_remove_obj(object);
        for(auto* object:_directory_focusables)lv_group_remove_obj(object);
    }
    lv_obj_add_flag(_screen,LV_OBJ_FLAG_HIDDEN);
}
void NomadNetScreen::clicked(lv_event_t* event){
    auto* self=static_cast<NomadNetScreen*>(lv_event_get_user_data(event));auto* target=lv_event_get_target(event);
    if(target==self->_back_button&&self->_back)self->_back();
    else if(target==self->_home_button&&self->_home)self->_home();
    else if(target==self->_reload_button&&self->_reload){const std::string address=self->address();if(self->_reload(address))self->begin_navigation(address);else self->set_status("Browser action queue is busy");}
    else if(target==self->_save_button&&self->_save){if(!self->_save(self->address()))self->set_status("Browser action queue is busy");}
    else if(target==self->_edit_button){self->set_address_editing(true);self->set_status("Edit destination or page path");}
    else if((target==self->_go_button||target==self->_address)&&self->_open){const std::string address=self->address();if(self->_open(address))self->begin_navigation(address);else self->set_status("Browser action queue is busy");}
    else{
        const std::size_t code=reinterpret_cast<std::size_t>(lv_obj_get_user_data(target));
        if(code==1001)self->render_directory(View::HEARD);
        else if(code==1002)self->render_directory(View::SAVED_NODES);
        else if(code==1003)self->render_directory(View::SAVED_PAGES);
        else if(code==1004)self->render_directory(View::RECENT);
        else if(code==1005){self->clear_document();self->show_browser(true);self->set_status("Enter a NomadNet address");}
        else if(code>2000&&code<=2000+self->_directory_targets.size()){
            const std::string selected=self->_directory_targets[code-2001];
            if(self->_open&&self->_open(selected))self->begin_navigation(selected);
            else self->set_status("Browser action queue is busy");
        }else if(code>0&&code<=self->_link_targets.size()&&self->_link){const std::string selected=self->_link_targets[code-1];if(self->_link(selected))self->begin_navigation(selected);else self->set_status("Browser action queue is busy");}
    }
}
}
#endif
