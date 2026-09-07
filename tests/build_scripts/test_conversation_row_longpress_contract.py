"""Source contract: conversation-row selection must not fire on long-press release.

LVGL 8.4 lv_indev.c indev_proc_release() (lines ~973-980) sends
LV_EVENT_CLICKED on EVERY pointer release without scrolling, including a
long-press release — only LV_EVENT_SHORT_CLICKED is gated on
long_pr_sent == 0. With both CLICKED and LONG_PRESSED bound to the
conversation row, a long-press to open the delete dialog would also, on
release, fire the selection handler and navigate into the conversation,
hiding the confirm dialog.

The row therefore binds selection to LV_EVENT_SHORT_CLICKED. Trackball
selection is unaffected: the keypad indev path sends SHORT_CLICKED and
CLICKED for an enter release without a long press, and suppresses both
when long_pr_sent is set.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIST_CPP = ROOT / "lib/tdeck_ui/UI/LXMF/ConversationListScreen.cpp"


def _source() -> str:
    return LIST_CPP.read_text()


def test_row_selection_uses_short_clicked():
    source = _source()
    assert (
        "lv_obj_add_event_cb(container, on_conversation_clicked, LV_EVENT_SHORT_CLICKED, this)"
        in source
    ), (
        "conversation row selection must bind LV_EVENT_SHORT_CLICKED — "
        "LV_EVENT_CLICKED fires on long-press release in LVGL 8.4 and "
        "would navigate into the chat while the delete dialog is open"
    )


def test_row_has_no_clicked_binding():
    source = _source()
    assert "on_conversation_clicked, LV_EVENT_CLICKED" not in source, (
        "stale LV_EVENT_CLICKED binding on the conversation row would "
        "re-introduce the long-press-release navigation"
    )


def test_row_long_press_delete_binding_unchanged():
    source = _source()
    assert (
        "lv_obj_add_event_cb(container, on_conversation_long_pressed, LV_EVENT_LONG_PRESSED, this)"
        in source
    ), "long-press delete on the conversation row must remain bound"
