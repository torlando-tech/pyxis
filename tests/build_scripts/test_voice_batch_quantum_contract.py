from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CAPTURE_HEADER = ROOT / "lib/lxst_audio/i2s_capture.h"
CAPTURE_CPP = ROOT / "lib/lxst_audio/i2s_capture.cpp"
UI_MANAGER_CPP = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"


def test_production_voice_is_ulbw_only():
    capture_header = CAPTURE_HEADER.read_text()
    capture_source = CAPTURE_CPP.read_text()
    ui_source = UI_MANAGER_CPP.read_text()

    assert "PCM_SAMPLES_PER_BATCH = 3200" in capture_header
    assert "ULBWVoiceProfilePolicy::CODEC2_MODE_700C_VALUE" in capture_source
    assert "UIManager::_preferred_profile = UIManager::LXST_PROFILE_ULBW" in ui_source
    assert "return ULBWVoiceProfilePolicy::codecModeForProfile(profile);" in ui_source


def test_ulbw_tx_uses_real_42_byte_batch_shape():
    ui_source = UI_MANAGER_CPP.read_text()

    assert "uint8_t encoded_buf[128]" in ui_source
    assert "uint8_t batch_data[128]" in ui_source
    assert "ULBWVoiceProfilePolicy::framesPerPacket(320)" in ui_source
