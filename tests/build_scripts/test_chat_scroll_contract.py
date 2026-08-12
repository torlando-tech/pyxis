"""Source-level contracts for opening a conversation at its newest message."""
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CHAT_CPP = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.cpp"
CHAT_H = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.h"


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_initial_background_fill_stays_at_newest_message():
    source = CHAT_CPP.read_text()
    header = CHAT_H.read_text()
    refresh = function_body(source, "void ChatScreen::refresh()", "void ChatScreen::tick_background_fill()")
    tick = function_body(source, "void ChatScreen::tick_background_fill()", "void ChatScreen::load_more_messages(")
    on_scroll = function_body(source, "void ChatScreen::on_scroll(", "void ChatScreen::create_message_bubble(")
    assert "void ChatScreen::scroll_to_bottom()" in source
    helper = function_body(source, "void ChatScreen::scroll_to_bottom()", "void ChatScreen::on_scroll(")

    assert "std::atomic<bool> _keep_bottom_during_background_fill" in header
    assert "void scroll_to_bottom();" in header
    assert helper.index("lv_obj_update_layout(_message_list)") < helper.index(
        "lv_obj_scroll_to_y(_message_list, LV_COORD_MAX, LV_ANIM_OFF)"
    )

    keep = refresh.index("_keep_bottom_during_background_fill.store(initial_fill_active)")
    activate = refresh.index("_bg_fill_active.store(initial_fill_active)")
    bottom = refresh.index("scroll_to_bottom()")
    assert keep < activate < bottom

    load = tick.index("load_more_messages(")
    keep_check = tick.index("if (_keep_bottom_during_background_fill.load())")
    bottom_after_fill = tick.index("scroll_to_bottom()", keep_check)
    assert load < keep_check < bottom_after_fill

    user_fill = on_scroll.index("_bg_fill_active.store(true)")
    stop_pinning = on_scroll.index("_keep_bottom_during_background_fill.store(false)")
    assert stop_pinning < user_fill


def test_show_resolves_hidden_layout_before_presenting_newest_message():
    source = CHAT_CPP.read_text()
    show = function_body(source, "void ChatScreen::show()", "void ChatScreen::hide()")
    unhide = show.index("lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN)")
    bottom = show.index("scroll_to_bottom()")
    assert unhide < bottom
