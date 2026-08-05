#include "NomadNetScreen.h"
#ifdef ARDUINO
#include "Theme.h"
#include "NomadNetDisplay.h"
#include "../LVGL/LVGLInit.h"
#include "../TextAreaHelper.h"
#include <algorithm>

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
    for(auto* button:{_back_button,_home_button,_reload_button}) {
        lv_obj_set_style_bg_color(button,Theme::surfaceContainer(),0);
        lv_obj_set_style_bg_color(button,Theme::primaryPressed(),LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(button,0,0);
        lv_obj_set_style_radius(button,8,0);
    }

    _address_row=lv_obj_create(_screen);lv_obj_set_size(_address_row,LV_PCT(100),38);lv_obj_align(_address_row,LV_ALIGN_TOP_MID,0,34);
    lv_obj_set_style_bg_color(_address_row,Theme::surface(),0);lv_obj_set_style_border_width(_address_row,0,0);lv_obj_set_style_pad_all(_address_row,3,0);
    _address=lv_textarea_create(_address_row);lv_obj_set_size(_address,270,30);lv_obj_align(_address,LV_ALIGN_LEFT_MID,0,0);
    lv_textarea_set_one_line(_address,true);lv_textarea_set_max_length(_address,511);lv_textarea_set_placeholder_text(_address,"destination:/page/path");
    lv_obj_set_style_text_font(_address,&lv_font_montserrat_12,0);lv_obj_set_style_bg_color(_address,Theme::surfaceInput(),0);TextAreaHelper::enable_paste(_address);
    _go_button=lv_btn_create(_address_row);lv_obj_set_size(_go_button,42,30);lv_obj_align(_go_button,LV_ALIGN_RIGHT_MID,0,0);
    lv_obj_set_style_bg_color(_go_button,Theme::primary(),0);lv_obj_set_style_radius(_go_button,8,0);
    lv_obj_t* gl=lv_label_create(_go_button);lv_label_set_text(gl,"Go");lv_obj_center(gl);

    _address_summary=lv_label_create(_address_row);lv_obj_set_size(_address_summary,266,24);lv_obj_align(_address_summary,LV_ALIGN_LEFT_MID,4,0);
    lv_label_set_long_mode(_address_summary,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(_address_summary,&lv_font_montserrat_12,0);
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
    lv_obj_set_scroll_dir(_content,LV_DIR_VER);lv_obj_set_scrollbar_mode(_content,LV_SCROLLBAR_MODE_AUTO);
    for(auto* o:{_back_button,_home_button,_reload_button,_go_button,_edit_button})lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,this);
    lv_obj_add_event_cb(_address,clicked,LV_EVENT_READY,this);
    set_status("Enter a NomadNet address");hide();
}
NomadNetScreen::~NomadNetScreen(){if(_screen)lv_obj_del(_screen);}
void NomadNetScreen::set_address(const std::string& value){
    lv_textarea_set_text(_address,value.c_str());
    const auto summary=NomadNet::compact_address(value);
    lv_label_set_text(_address_summary,summary.c_str());
}
std::string NomadNetScreen::address()const{return lv_textarea_get_text(_address);}
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
            lv_span_set_text(span, run.text.c_str());
            const bool is_link = run.link_index >= 0 &&
                static_cast<std::size_t>(run.link_index) < document.links.size();
            lv_style_set_text_color(&span->style, is_link ? Theme::primaryLight() :
                run.has_foreground ? lv_color_hex(run.foreground) :
                document.has_foreground ? lv_color_hex(document.foreground) : Theme::textPrimary());
            lv_style_set_text_font(&span->style,
                (run.bold || block.type == NomadNet::BlockType::HEADING)
                    ? &lv_font_montserrat_16 : &lv_font_montserrat_12);
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
            const std::string caption = "Open: " + run.text;
            lv_label_set_text(label, caption.c_str());
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(label, 294);
            lv_obj_center(label);
            _link_targets.push_back(document.links[run.link_index].target);
            lv_obj_set_user_data(button, reinterpret_cast<void*>(_link_targets.size()));
            lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, this);
            _focusables.push_back(button);
            if (_visible && group) lv_group_add_obj(group, button);
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
    set_address_editing(false);
    if(_visible&&group&&!_focusables.empty())lv_group_focus_obj(_focusables.front());
}
void NomadNetScreen::show(){
    _visible=true;lv_obj_clear_flag(_screen,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(_screen);
    auto* group=LVGL::LVGLInit::get_default_group();
    if(!group)return;
    for(auto* object:{_back_button,_home_button,_reload_button})lv_group_add_obj(group,object);
    if(_editing){lv_group_add_obj(group,_address);lv_group_add_obj(group,_go_button);}
    else lv_group_add_obj(group,_edit_button);
    for(auto* object:_focusables)lv_group_add_obj(group,object);
    if(!_editing&&!_focusables.empty())lv_group_focus_obj(_focusables.front());
    else lv_group_focus_obj(_editing?_address:_edit_button);
}
void NomadNetScreen::hide(){
    _visible=false;auto* group=LVGL::LVGLInit::get_default_group();
    if(group){
        for(auto* object:{_back_button,_home_button,_reload_button,_address,_go_button,_edit_button})lv_group_remove_obj(object);
        for(auto* object:_focusables)lv_group_remove_obj(object);
    }
    lv_obj_add_flag(_screen,LV_OBJ_FLAG_HIDDEN);
}
void NomadNetScreen::clicked(lv_event_t* event){
    auto* self=static_cast<NomadNetScreen*>(lv_event_get_user_data(event));auto* target=lv_event_get_target(event);
    if(target==self->_back_button&&self->_back)self->_back();
    else if(target==self->_home_button&&self->_home)self->_home();
    else if(target==self->_reload_button&&self->_reload)self->_reload();
    else if(target==self->_edit_button){self->set_address_editing(true);self->set_status("Edit destination or page path");}
    else if((target==self->_go_button||target==self->_address)&&self->_open)self->_open(self->address());
    else{
        const std::size_t index=reinterpret_cast<std::size_t>(lv_obj_get_user_data(target));
        if(index>0&&index<=self->_link_targets.size()&&self->_link)self->_link(self->_link_targets[index-1]);
    }
}
}
#endif
