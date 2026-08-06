"""Compile and execute portable directional trackball navigation regressions."""

from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def test_trackball_navigation(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        raise RuntimeError("A C++17 compiler is required for this test")

    binary = tmp_path / "trackball_navigation"
    result = subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(ROOT / "tests/native/test_trackball_navigation.cpp"),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr

    run = subprocess.run([str(binary)], text=True, capture_output=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "4 trackball navigation tests passed" in run.stdout


def test_trackball_and_nomadnet_directional_integration_contract():
    trackball = (ROOT / "lib/tdeck_ui/Hardware/TDeck/Trackball.cpp").read_text()
    nomadnet = (ROOT / "lib/tdeck_ui/UI/LXMF/NomadNetScreen.cpp").read_text()

    assert "LV_KEY_NEXT" not in trackball
    assert "LV_KEY_PREV" not in trackball
    assert "NavigationDirection::UP" in trackball
    assert "NavigationDirection::DOWN" in trackball
    assert "NavigationDirection::LEFT" in trackball
    assert "NavigationDirection::RIGHT" in trackball
    assert "navigate_or_scroll" in trackball
    # Modal dialogs temporarily move the trackball to an isolated group. The
    # callback must navigate that assigned group, never the global default.
    assert "_indev ? _indev->group : nullptr" in trackball
    # Clipped/off-screen controls are not directional candidates. Scrolling
    # reveals them before they can receive focus.
    assert "lv_obj_is_visible(object)" in trackball
    assert "if (!focused || object_is_hidden(focused))" in trackball
    assert "if (!focused || !object_is_visible(focused))" not in trackball
    # Generic scroll fallback is scoped to the assigned group's common UI
    # subtree, so modal input cannot scroll the screen behind it.
    assert "group_navigation_root(group, focused)" in trackball
    assert "find_scroll_target(lv_scr_act()" not in trackball
    assert "if (group->frozen) return false;" in trackball
    assert "!lv_obj_has_state(object, LV_STATE_DISABLED)" in trackball
    # Physical T-Deck evidence establishes that positive Y pulses are upward;
    # keep horizontal polarity unchanged. Navigation is one pulse sensitive and
    # retains pulses received during the repeat-rate window instead of dropping them.
    config = (ROOT / "lib/tdeck_ui/Hardware/TDeck/Config.h").read_text()
    assert "NAV_THRESHOLD = 1" in config
    assert "KEY_REPEAT_MS = 60" in config
    assert "accum_y > 0 ? NavigationDirection::UP : NavigationDirection::DOWN" in trackball
    assert "accum_x > 0 ? NavigationDirection::RIGHT : NavigationDirection::LEFT" in trackball
    assert trackball.count("last_key_time = now;\n            accum_x = 0;\n            accum_y = 0;") == 2
    # One LVGL group has exactly one focus owner. Normalize stale focus states
    # observed physically so two buttons cannot remain highlighted together.
    assert "normalize_group_focus_state(group);" in trackball
    assert "LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY" in trackball
    assert "LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY | LV_STATE_EDITED" not in trackball

    # NomadNet's document viewport must remain a bounded vertical scroll target;
    # trackball movement reaches it through the generic LVGL navigation helper.
    assert "lv_obj_set_scroll_dir(_content,LV_DIR_VER)" in nomadnet
    assert "LV_OBJ_FLAG_SCROLLABLE" in nomadnet
