from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.h"
SOURCE = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
CODEC_SOURCE = ROOT / "lib/lxst_audio/codec_wrapper.cpp"


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def test_voice_liveness_watchdog_is_conservative_and_wired_to_media():
    header = HEADER.read_text()
    source = SOURCE.read_text()
    update = _function_body(source, "void UIManager::call_update()")
    receive = _function_body(source, "void UIManager::call_rx_audio_frame")

    assert "CALL_MEDIA_LIVENESS_TIMEOUT_MS = 90000" in header
    assert source.count("_call_liveness.arm(_call_start_ms);") == 3
    assert "frame_len < 2" in receive
    assert re.search(
        r"if\s*\(_lxst_audio->writeEncodedPacket\([^;]+\)\)\s*\{[^}]*"
        r"_call_liveness\.observe\(millis\(\)\);",
        receive,
        re.DOTALL,
    )
    assert "_call_liveness.expired(now, CALL_MEDIA_LIVENESS_TIMEOUT_MS)" in update
    assert "call_ended();" in update


def test_every_owner_cleanup_disarms_liveness_before_releasing_generation():
    source = SOURCE.read_text()
    for signature in ("void UIManager::call_hangup()", "void UIManager::call_ended()"):
        body = _function_body(source, signature)
        assert body.index("_call_liveness.disarm();") < body.index("call_clear_generation(generation);")


def test_codec2_decoder_rejects_empty_and_partial_subframe_batches():
    source = CODEC_SOURCE.read_text()
    decode = _function_body(source, "int Codec2Wrapper::decode")

    validation = decode.index("dataLen <= 0 || bytesPerFrame_ <= 0 || dataLen % bytesPerFrame_ != 0")
    frame_count = decode.index("int numFrames = dataLen / bytesPerFrame_;")
    assert validation < frame_count
