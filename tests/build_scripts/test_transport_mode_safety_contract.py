from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "src/main.cpp").read_text()
SETTINGS_H = (ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.h").read_text()
SETTINGS_CPP = (ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.cpp").read_text()


def test_transport_mode_defaults_off_and_controls_reticulum_startup():
    assert "bool transport_enabled;" in SETTINGS_H
    assert "transport_enabled(false)" in SETTINGS_H
    assert 'prefs.getBool("transport", false)' in MAIN
    assert MAIN.index("load_app_settings();") < MAIN.index("setup_reticulum();")
    assert "Reticulum::transport_enabled(app_settings.transport_enabled);" in MAIN
    assert "Reticulum::transport_enabled(true);" not in MAIN


def test_transport_mode_setting_is_persisted():
    assert 'KEY_TRANSPORT_ENABLED = "transport"' in SETTINGS_CPP
    assert "prefs.getBool(KEY_TRANSPORT_ENABLED, false)" in SETTINGS_CPP
    assert "prefs.putBool(KEY_TRANSPORT_ENABLED, _settings.transport_enabled)" in SETTINGS_CPP


def test_transport_toggle_is_last_and_requires_explicit_warning_confirmation():
    advanced = SETTINGS_CPP.index("create_advanced_section(_content);")
    transport = SETTINGS_CPP.index("create_transport_mode_section(_content);")
    assert advanced < transport

    assert "DANGER: Transport Mode" in SETTINGS_CPP
    assert "NOT RECOMMENDED" in SETTINGS_CPP
    assert "saturate LoRa airtime" in SETTINGS_CPP
    assert "drain the battery" in SETTINGS_CPP
    assert "ENABLE ANYWAY" in SETTINGS_CPP
    assert "Takes effect after reboot" in SETTINGS_CPP
    assert "on_transport_enabled_changed" in SETTINGS_CPP
    assert "on_transport_confirm_enable" in SETTINGS_CPP
    assert "lv_group_create()" in SETTINGS_CPP
    assert "lv_indev_set_group(keyboard, _transport_modal_group)" in SETTINGS_CPP
    assert "lv_indev_set_group(trackball, _transport_modal_group)" in SETTINGS_CPP
