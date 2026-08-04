import shutil
import subprocess
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TEST_SOURCE = HERE / "test_radio_activity.cpp"
HISTORY_INCLUDE = ROOT / "lib" / "radio_activity"
SX_CPP = ROOT / "lib" / "sx1262_interface" / "SX1262Interface.cpp"
SX_H = ROOT / "lib" / "sx1262_interface" / "SX1262Interface.h"
MAIN_CPP = ROOT / "src" / "main.cpp"
UI_MANAGER_CPP = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF" / "UIManager.cpp"
UI_MANAGER_H = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF" / "UIManager.h"
SCREEN_CPP = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF" / "RadioActivityScreen.cpp"
STATUS_CPP = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF" / "StatusScreen.cpp"


def _find_cxx():
    for command in ("clang++", "g++"):
        if shutil.which(command):
            return command
    pytest.skip("no C++ compiler found")


def test_radio_activity_history_native(tmp_path):
    binary = tmp_path / "test_radio_activity"
    command = [
        _find_cxx(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{HISTORY_INCLUDE}",
        str(TEST_SOURCE),
        "-o",
        str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr

    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "13 passed, 0 failed"


def test_sampler_is_main_loop_owned_bounded_and_instantaneous():
    sx_cpp = SX_CPP.read_text()
    sx_h = SX_H.read_text()
    main_cpp = MAIN_CPP.read_text()

    assert "sample_radio_activity" in sx_h
    assert "_radio->getRSSI(false)" in sx_cpp
    assert "xSemaphoreTake(_spi_mutex, 0)" in sx_cpp
    assert "if (_transmitting)" in sx_cpp
    assert "sample_radio_activity" in main_cpp
    assert "radio_activity_visible()" in main_cpp
    assert "setFrequency" not in sx_cpp


def test_ui_contract_uses_dedicated_status_child_and_snapshot_only_rendering():
    manager_cpp = UI_MANAGER_CPP.read_text()
    manager_h = UI_MANAGER_H.read_text()
    screen_cpp = SCREEN_CPP.read_text()
    status_cpp = STATUS_CPP.read_text()

    assert "SCREEN_RADIO_ACTIVITY" in manager_h
    assert "set_radio_activity_callback" in status_cpp
    assert "show_radio_activity" in manager_cpp
    assert "on_back_from_radio_activity" in manager_cpp
    assert "Radio Activity" in screen_cpp
    assert "LIVE · 7 FPS" in screen_cpp
    assert "RENDER_INTERVAL_MS = 143" in screen_cpp
    assert "lv_canvas" not in screen_cpp
    assert "lv_chart" not in screen_cpp
    assert "lv_draw_line" in screen_cpp
    assert "getRSSI" not in screen_cpp
    assert "SPI" not in screen_cpp
    assert "setFrequency" not in screen_cpp
