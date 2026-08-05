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
    assert result.stdout.strip() == "33 passed, 0 failed"


def test_sampler_is_main_loop_owned_bounded_and_instantaneous():
    sx_cpp = SX_CPP.read_text()
    sx_h = SX_H.read_text()
    main_cpp = MAIN_CPP.read_text()

    assert "sample_radio_activity" in sx_h
    assert "_radio->getRSSI(false)" in sx_cpp
    assert "xSemaphoreTake(_spi_mutex, 0)" in sx_cpp
    assert "if (_transmitting)" in sx_cpp
    assert "record_gap" in sx_cpp
    assert "elapsed / ACTIVITY_SAMPLE_INTERVAL_MS" in sx_cpp
    assert "_last_activity_sample_ms = last_activity_sample_ms +" in sx_cpp
    assert "portENTER_CRITICAL(&_activity_mux)" not in sx_cpp
    assert "xSemaphoreTake(_activity_mutex" in sx_cpp
    assert sx_cpp.count("lock_activity(portMAX_DELAY)") == 5
    assert "sample_radio_activity" in main_cpp
    assert "radio_activity_visible()" in main_cpp
    assert "setFrequency" not in sx_cpp


def test_radio_reconfiguration_resets_current_channel_history_and_clock():
    sx_cpp = SX_CPP.read_text()
    set_config_start = sx_cpp.index("void SX1262Interface::set_config(")
    set_config_end = sx_cpp.index("std::string SX1262Interface::toString()")
    set_config = sx_cpp[set_config_start:set_config_end]

    assert "lock_activity(portMAX_DELAY)" in set_config
    assert "_activity_history.reset()" in set_config
    assert "_last_activity_sample_ms = millis()" in set_config
    assert set_config.index("lock_activity(portMAX_DELAY)") < set_config.index("_activity_history.reset()")
    assert set_config.index("_activity_history.reset()") < set_config.index("unlock_activity()")


def test_stale_radio_operations_cannot_repopulate_a_new_channel_generation():
    sx_cpp = SX_CPP.read_text()
    sampler = sx_cpp[sx_cpp.index("void SX1262Interface::sample_radio_activity("):
                     sx_cpp.index("RadioActivity::Snapshot SX1262Interface::radio_activity_snapshot()")]
    receiver = sx_cpp[sx_cpp.index("void SX1262Interface::loop()"):
                      sx_cpp.index("bool SX1262Interface::send_outgoing(")]
    transmitter = sx_cpp[sx_cpp.index("bool SX1262Interface::send_outgoing("):
                         sx_cpp.index("void SX1262Interface::on_incoming(")]

    for operation in (sampler, receiver, transmitter):
        assert "activity_generation = _activity_history.generation()" in operation
        assert "_activity_history.generation() != activity_generation" in operation

    initial_lock = sampler.index("lock_activity(pdMS_TO_TICKS(2))")
    refreshed_now = sampler.index("now_ms = millis()")
    initial_unlock = sampler.index("unlock_activity()")
    assert initial_lock < refreshed_now < initial_unlock

    first_generation_check = sampler.index("_activity_history.generation() != activity_generation")
    second_generation_check = sampler.index("_activity_history.generation() != activity_generation", first_generation_check + 1)
    first_clock_advance = sampler.index("advance_clock();")
    second_clock_advance = sampler.index("advance_clock();", first_clock_advance + 1)
    assert first_generation_check < first_clock_advance
    assert second_generation_check < second_clock_advance
    assert receiver.index("_activity_history.generation() != activity_generation") < receiver.index("mark_event(RadioActivity::Event::Rx)")
    assert transmitter.index("_activity_history.generation() != activity_generation") < transmitter.index("mark_event(RadioActivity::Event::Tx")


def test_ui_contract_uses_dedicated_status_child_and_snapshot_only_rendering():
    manager_cpp = UI_MANAGER_CPP.read_text()
    manager_h = UI_MANAGER_H.read_text()
    screen_cpp = SCREEN_CPP.read_text()
    status_cpp = STATUS_CPP.read_text()

    assert "Route::RADIO_ACTIVITY" in manager_h
    assert "set_radio_activity_callback" in status_cpp
    assert "show_radio_activity" in manager_cpp
    assert "on_back_from_radio_activity" in manager_cpp
    assert "Radio Activity" in screen_cpp
    assert "LIVE | 7 FPS" in screen_cpp
    assert "RENDER_INTERVAL_MS = 143" in screen_cpp
    assert "render_due" in screen_cpp
    assert manager_cpp.index("render_due(now)") < manager_cpp.index("_radio_activity_snapshot_provider()")
    assert "lv_canvas" not in screen_cpp
    assert "lv_chart" not in screen_cpp
    assert "lv_draw_line" in screen_cpp
    assert "rssi_valid" in screen_cpp
    assert "current_rssi_valid" in screen_cpp
    assert '"-- dBm"' in screen_cpp
    assert " · " not in screen_cpp
    assert "■" not in screen_cpp
    assert " | " in screen_cpp
    assert "getRSSI" not in screen_cpp
    assert "SPI" not in screen_cpp
    assert "setFrequency" not in screen_cpp
    assert "legend_noise" in screen_cpp
    assert "legend_rx" in screen_cpp
    assert "legend_other" in screen_cpp
    assert "legend_tx" in screen_cpp
