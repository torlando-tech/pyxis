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
    prepare = function_body(source, "void ChatScreen::prepare_conversation()", "void ChatScreen::refresh()")
    tick = function_body(source, "void ChatScreen::tick_background_fill()", "void ChatScreen::load_more_messages(")
    on_scroll = function_body(source, "void ChatScreen::on_scroll(", "void ChatScreen::create_message_bubble(")
    assert "void ChatScreen::scroll_to_bottom()" in source
    helper = function_body(source, "void ChatScreen::scroll_to_bottom()", "void ChatScreen::on_scroll(")

    assert "std::atomic<bool> _keep_bottom_during_background_fill" in header
    assert "void scroll_to_bottom();" in header
    assert helper.index("lv_obj_update_layout(_message_list)") < helper.index(
        "lv_obj_scroll_to_y(_message_list, LV_COORD_MAX, LV_ANIM_OFF)"
    )

    # Cold-open I/O (identity recall, display name, message index, per-message
    # metadata) runs on the main loop in prepare_conversation, between the
    # guard lock and the commit lock — never inside a held LVGL lock, so a
    # slow LittleFS open can't hold the mutex past the 5s deadlock guard.
    i_io = prepare.index("Identity::recall_app_data")
    i_index = prepare.index("get_messages_for_conversation")
    i_meta = prepare.index("load_message_metadata")
    i_commit_lock = prepare.index("commit, brief LVGL lock")
    i_rows = prepare.index("create_message_bubble(item)")
    i_target = prepare.index("_bg_fill_target =")
    i_keep = prepare.index("_keep_bottom_during_background_fill.store(initial_fill_active)")
    i_activate = prepare.index("_bg_fill_active.store(initial_fill_active)")
    i_bottom = prepare.index("scroll_to_bottom()")
    assert i_commit_lock < i_rows < i_target < i_keep < i_activate < i_bottom
    assert i_io < i_index < i_meta < i_commit_lock
    # refresh() must not do the slow store reads itself (it just re-arms the
    # prepare; the main loop does the I/O off-lock).
    refresh = function_body(source, "void ChatScreen::refresh()", "void ChatScreen::tick_background_fill()")
    assert "load_message_metadata" not in refresh
    assert "get_messages_for_conversation" not in refresh

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
