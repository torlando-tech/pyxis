"""Regression coverage for lossless display refresh after SPI contention."""

from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def test_display_refresh_retry_state(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        raise RuntimeError("A C++17 compiler is required")

    binary = tmp_path / "display_refresh_retry"
    subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT),
            str(ROOT / "tests/native/test_display_refresh_retry.cpp"),
            "-o",
            str(binary),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(binary)], check=True)


def test_display_retry_is_wired_after_lvgl_refresh():
    display = (ROOT / "lib/tdeck_ui/Hardware/TDeck/Display.cpp").read_text()
    lvgl = (ROOT / "lib/tdeck_ui/UI/LVGL/LVGLInit.cpp").read_text()
    trackball = (ROOT / "lib/tdeck_ui/Hardware/TDeck/Trackball.cpp").read_text()

    assert "refresh_retry.mark_failed();" in display
    assert "Skip this frame — LVGL will retry next tick" not in display
    assert "Display::consume_refresh_retry()" in lvgl
    assert "lv_obj_invalidate(screen)" in lvgl

    # Focus state belongs to LVGL's group machinery. The display retry fixes
    # stale LCD pixels without mutating non-owner widget state.
    assert "normalize_group_focus_state" not in trackball
