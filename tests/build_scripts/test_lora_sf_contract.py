from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SETTINGS_CPP = (ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.cpp").read_text()
SETTINGS_H = (ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.h").read_text()
SX1262_H = (ROOT / "lib/sx1262_interface/SX1262Interface.h").read_text()


def test_lora_settings_expose_the_full_sx1262_sf5_through_sf12_range():
    assert 'lv_dropdown_set_options(_dropdown_lora_sf, "5\\n6\\n7\\n8\\n9\\n10\\n11\\n12");' in SETTINGS_CPP
    assert "lv_dropdown_set_selected(_dropdown_lora_sf, _settings.lora_sf - 5);" in SETTINGS_CPP
    assert "_settings.lora_sf = lv_dropdown_get_selected(_dropdown_lora_sf) + 5;" in SETTINGS_CPP


def test_lora_spreading_factor_documentation_matches_the_supported_range():
    assert "Spreading factor (5-12)" in SETTINGS_H
    assert "SF5-SF12" in SX1262_H
