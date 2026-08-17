from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCREEN = ROOT / "lib/tdeck_ui/UI/LXMF/NomadNetScreen.cpp"
KEYBOARD = ROOT / "lib/tdeck_ui/Hardware/TDeck/Keyboard.cpp"


def test_physical_enter_is_normalized_and_commits_multiline_field_editor():
    keyboard = KEYBOARD.read_text()
    screen = SCREEN.read_text()

    assert "key==KEY_ENTER?LV_KEY_ENTER:key" in keyboard
    begin = screen[screen.index("void NomadNetScreen::begin_field_edit"):screen.index(
        "void NomadNetScreen::finish_field_edit"
    )]
    event = screen[screen.index("void NomadNetScreen::field_editor_event"):screen.index(
        "void NomadNetScreen::page_event"
    )]
    assert "lv_textarea_set_one_line(_field_editor,type==NomadNet::FormFieldType::PASSWORD)" in begin
    assert "type==NomadNet::FormFieldType::PASSWORD?38:76" in begin
    assert "static_cast<lv_event_code_t>(LV_EVENT_ALL|LV_EVENT_PREPROCESS)" in begin
    assert "LVGL::LVGLInit::get_keyboard()" in event
    assert "key==LV_KEY_ENTER" in event
    assert "lv_event_stop_processing(event)" in event
    assert "finish_editor(true)" in event


def test_trackball_enter_remains_available_for_newline_in_multiline_editor():
    screen = SCREEN.read_text()
    event = screen[screen.index("void NomadNetScreen::field_editor_event"):screen.index(
        "void NomadNetScreen::page_event"
    )]

    assert "LVGL::LVGLInit::get_trackball()" in event
    assert "lv_textarea_get_one_line(editor)" in event
    assert "finish_editor(true)" in event
    assert "lv_textarea_add_char(editor,'\\n')" in event
    assert event.count("lv_event_stop_processing(event)") >= 2


def test_editor_is_detached_from_active_indev_before_synchronous_delete():
    screen = SCREEN.read_text()
    event = screen[screen.index("void NomadNetScreen::field_editor_event"):screen.index(
        "void NomadNetScreen::page_event"
    )]

    finish = event[event.index("auto finish_editor="):event.index("const auto code=")]
    assert "lv_indev_get_act()" in finish
    assert "lv_indev_reset(indev,editor)" in finish
    assert finish.index("lv_indev_reset(indev,editor)") < finish.index(
        "self->finish_field_edit(accept)"
    )
    assert "if(code==LV_EVENT_READY)finish_editor(true)" in event
    assert "else if(code==LV_EVENT_CANCEL)finish_editor(false)" in event
    assert "else if(key==LV_KEY_ESC)finish_editor(false)" in event


def test_choice_field_width_uses_exact_prefix_glyph_width():
    screen = SCREEN.read_text()
    table_layout = screen[screen.index("bool NomadNetScreen::layout_table_cell"):screen.index(
        "bool NomadNetScreen::layout_table_fit"
    )]
    table_measurement = screen[screen.index("bool NomadNetScreen::layout_table("):screen.index(
        "bool NomadNetScreen::layout_from"
    )]
    block_layout = screen[screen.index("bool NomadNetScreen::layout_from"):screen.index(
        "bool NomadNetScreen::layout_window"
    )]

    for layout in (table_layout, table_measurement, block_layout):
        assert 'const char* prefix=field.type==NomadNet::FormFieldType::RADIO?"( ) ":"[ ] ";' in layout
        assert "lv_txt_get_width(prefix,4,font,0,LV_TEXT_FLAG_NONE)" in layout
        assert "requested=8+prefix_width+" in layout
        assert "requested=26+" not in layout
