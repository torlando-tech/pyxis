"""
Shared fixtures for LXST interop tests.

These tests verify wire-format and codec compatibility between:
  - Pyxis (ESP32 C++ implementation)
  - Python LXST reference implementation
  - LXST-kt (Kotlin/Android) — same wire format as Python LXST
"""

import sys
import math
import struct
import numpy as np
import pytest

# Add Python LXST to path
sys.path.insert(0, "$HOME/repos/public/LXST")

import pycodec2
from RNS.vendor import umsgpack as msgpack

# ── Constants matching all three implementations ──

# Msgpack field keys (Network.py, UIManager.cpp, PacketRouter.kt)
FIELD_SIGNALLING = 0x00
FIELD_FRAMES = 0x01

# Codec type header bytes (Codecs/__init__.py, Packetizer.kt, UIManager.h)
CODEC_NULL = 0xFF
CODEC_RAW = 0x00
CODEC_OPUS = 0x01
CODEC_CODEC2 = 0x02

# Codec2 mode headers (Codec2.py, Codec2.kt, codec_wrapper.cpp)
MODE_HEADERS = {
    700: 0x00, 1200: 0x01, 1300: 0x02, 1400: 0x03,
    1600: 0x04, 2400: 0x05, 3200: 0x06,
}
HEADER_MODES = {v: k for k, v in MODE_HEADERS.items()}

# Codec2 library mode mapping (matches codec_wrapper.cpp headerToLibraryMode)
MODE_TO_LIBRARY = {
    3200: 0, 2400: 1, 1600: 2, 1400: 3, 1300: 4, 1200: 5, 700: 8,
}

# Signalling constants (UIManager.h, Signalling in LXST-kt)
STATUS_BUSY = 0x00
STATUS_REJECTED = 0x01
STATUS_CALLING = 0x02
STATUS_AVAILABLE = 0x03
STATUS_RINGING = 0x04
STATUS_CONNECTING = 0x05
STATUS_ESTABLISHED = 0x06
PREFERRED_PROFILE = 0xFF

# Profile IDs (Profile.kt)
PROFILE_ULBW = 0x10
PROFILE_VLBW = 0x20
PROFILE_LBW = 0x30
PROFILE_MQ = 0x40


@pytest.fixture
def codec2_3200():
    """Create a pycodec2 instance for 3200 bps mode."""
    return pycodec2.Codec2(3200)


@pytest.fixture
def codec2_1600():
    """Create a pycodec2 instance for 1600 bps mode."""
    return pycodec2.Codec2(1600)


@pytest.fixture
def lxst_codec2_3200():
    """Create a Python LXST Codec2 instance for 3200 bps."""
    from LXST.Codecs.Codec2 import Codec2
    return Codec2(mode=3200)


@pytest.fixture
def lxst_codec2_1600():
    """Create a Python LXST Codec2 instance for 1600 bps."""
    from LXST.Codecs.Codec2 import Codec2
    return Codec2(mode=1600)


@pytest.fixture
def sine_8khz_200ms():
    """Generate 200ms of 440Hz sine wave at 8kHz sample rate (int16)."""
    sr = 8000
    duration = 0.2
    t = np.arange(int(sr * duration)) / sr
    samples = (np.sin(2 * np.pi * 440 * t) * 16000).astype(np.int16)
    return samples


@pytest.fixture
def sine_8khz_1s():
    """Generate 1 second of 440Hz sine wave at 8kHz sample rate (int16)."""
    sr = 8000
    duration = 1.0
    t = np.arange(int(sr * duration)) / sr
    samples = (np.sin(2 * np.pi * 440 * t) * 16000).astype(np.int16)
    return samples


def codec2_mode_header(codec2):
    """
    Get the wire-format mode header byte for a pycodec2 instance.
    pycodec2 doesn't expose the mode directly, so we infer from bytes_per_frame
    and samples_per_frame.
    """
    spf = codec2.samples_per_frame()
    bpf = codec2.bytes_per_frame()
    # Map (spf, bpf) to mode header
    KNOWN_PARAMS = {
        (160, 8): 0x06,   # 3200
        (160, 8): 0x06,   # 3200
        (320, 8): 0x04,   # 1600
        (320, 7): 0x03,   # 1400
        (320, 7): 0x02,   # 1300 (same bpf as 1400 but different params)
        (320, 6): 0x01,   # 1200
        (320, 4): 0x00,   # 700C
    }
    # This is ambiguous for some modes. Use a different approach:
    # Try each mode and check SPF match
    for mode, header in MODE_HEADERS.items():
        lib_mode = MODE_TO_LIBRARY[mode]
        test_codec = pycodec2.Codec2(mode)
        if test_codec.samples_per_frame() == spf and test_codec.bytes_per_frame() == bpf:
            # Could be multiple matches (1300 vs 1400 both have spf=320, bpf=7)
            # Return first match — caller should ensure they pass the right codec
            return header
    raise ValueError(f"Unknown codec2 params: spf={spf} bpf={bpf}")


def encode_codec2_subframes(codec2, pcm_int16, mode_header=None):
    """
    Encode PCM samples into individual Codec2 sub-frames.

    Simulates Pyxis i2s_capture.cpp + codec_wrapper.cpp:
    Each encode produces [mode_header(1)] + [raw_encoded(N)] bytes.

    pycodec2.encode() returns only raw bytes, so we prepend the mode header
    to match what Pyxis codec_wrapper.cpp does.

    Returns list of (1 + bytes_per_frame)-byte encoded frames.
    """
    spf = codec2.samples_per_frame()
    bpf = codec2.bytes_per_frame()
    n_frames = len(pcm_int16) // spf

    if mode_header is None:
        mode_header = codec2_mode_header(codec2)

    frames = []
    for i in range(n_frames):
        chunk = pcm_int16[i * spf:(i + 1) * spf]
        raw_encoded = codec2.encode(chunk)
        # Prepend mode header (matches Pyxis codec_wrapper.cpp encode())
        frame = bytes([mode_header]) + raw_encoded
        frames.append(frame)
    return frames


def batch_subframes_pyxis_style(subframes, mode_header):
    """
    Batch multiple Codec2 sub-frames into Pyxis wire format.

    Simulates UIManager.cpp call_send_audio_batch TX batching:
    - First frame: keep [mode_header] + [data]
    - Subsequent frames: strip mode header, append raw data only
    - Result: [mode_header(1)] + [N * bytes_per_frame]

    Returns the batch_data bytes (without codec_type prefix).
    """
    if not subframes:
        return b""

    # First frame keeps header + data
    batch = bytearray(subframes[0])

    # Subsequent frames: strip 1-byte mode header
    for frame in subframes[1:]:
        batch.extend(frame[1:])  # skip mode header byte

    return bytes(batch)


def build_pyxis_audio_packet(batch_data):
    """
    Build a complete Pyxis-format wire packet for audio.

    Simulates UIManager.cpp call_send_audio_batch:
      {0x01: bin8([codec_type(0x02)] + batch_data)}

    Where batch_data = [mode_header] + [N * raw_codec2_bytes]
    """
    # Prepend codec type byte
    frame_bytes = bytes([CODEC_CODEC2]) + batch_data
    packet_data = {FIELD_FRAMES: frame_bytes}
    return msgpack.packb(packet_data)


def build_columba_audio_packet(codec_type_byte, encoded_frame):
    """
    Build a Columba/LXST-kt format wire packet for audio.

    Simulates Packetizer.kt + Python LXST Packetizer:
      {0x01: bytes([codec_type] + encoded_frame)}

    Where encoded_frame = [mode_header] + [N * raw_codec2_bytes]
    """
    frame_bytes = bytes([codec_type_byte]) + encoded_frame
    packet_data = {FIELD_FRAMES: frame_bytes}
    return msgpack.packb(packet_data)


def build_signal_packet(signal_value):
    """
    Build a signalling packet matching all implementations.

    Format: {0x00: [signal_value]}
    """
    packet_data = {FIELD_SIGNALLING: [signal_value]}
    return msgpack.packb(packet_data)


def parse_pyxis_rx(wire_bytes):
    """
    Simulate Pyxis call_on_packet() parsing.

    Parses the raw msgpack bytes exactly as UIManager.cpp does:
    - Check for fixmap(1): 0x81
    - Field 0x00 = signalling, field 0x01 = audio
    - Audio: bin8/bin16 → single frame, fixarray → batched frames
    - Returns list of (codec_type, codec_data) tuples for audio frames
    - Returns list of signal values for signalling packets
    """
    buf = wire_bytes
    if len(buf) < 4:
        return {"error": "packet too short"}

    if buf[0] != 0x81:
        return {"error": f"expected fixmap(1), got 0x{buf[0]:02x}"}

    field = buf[1]
    result = {"field": field, "frames": [], "signals": []}

    if field == FIELD_SIGNALLING:
        # {0x00: [signal]}
        if buf[2] != 0x91:
            return {"error": f"expected fixarray(1), got 0x{buf[2]:02x}"}
        if buf[3] <= 0x7F:
            result["signals"].append(buf[3])
        elif buf[3] == 0xCC and len(buf) >= 5:
            result["signals"].append(buf[4])
        elif buf[3] == 0xCD and len(buf) >= 6:
            result["signals"].append((buf[4] << 8) | buf[5])
        else:
            return {"error": f"unparseable signal 0x{buf[3]:02x}"}

    elif field == FIELD_FRAMES:
        fmt = buf[2]
        if (fmt & 0xF0) == 0x90:
            # fixarray: batched frames
            array_len = fmt & 0x0F
            pos = 3
            for _ in range(array_len):
                if pos >= len(buf):
                    break
                if buf[pos] == 0xC4:
                    frame_len = buf[pos + 1]
                    frame_start = pos + 2
                elif buf[pos] == 0xC5:
                    frame_len = (buf[pos + 1] << 8) | buf[pos + 2]
                    frame_start = pos + 3
                else:
                    break
                if frame_start + frame_len > len(buf) or frame_len < 2:
                    break
                frame = buf[frame_start:frame_start + frame_len]
                codec_type = frame[0]
                codec_data = frame[1:]
                result["frames"].append((codec_type, bytes(codec_data)))
                pos = frame_start + frame_len

        elif fmt == 0xC4:
            # bin8: single frame
            if len(buf) < 5:
                return {"error": "bin8 too short"}
            frame_len = buf[3]
            if len(buf) < 4 + frame_len or frame_len < 2:
                return {"error": "bin8 frame truncated"}
            frame = buf[4:4 + frame_len]
            codec_type = frame[0]
            codec_data = frame[1:]
            result["frames"].append((codec_type, bytes(codec_data)))

        elif fmt == 0xC5:
            # bin16: single frame
            if len(buf) < 6:
                return {"error": "bin16 too short"}
            frame_len = (buf[3] << 8) | buf[4]
            if len(buf) < 5 + frame_len or frame_len < 2:
                return {"error": "bin16 frame truncated"}
            frame = buf[5:5 + frame_len]
            codec_type = frame[0]
            codec_data = frame[1:]
            result["frames"].append((codec_type, bytes(codec_data)))

    return result


def parse_lxst_python_rx(wire_bytes):
    """
    Simulate Python LXST LinkSource._packet() parsing.

    This is the actual reference implementation logic from Network.py.
    Returns list of (codec_type_int, raw_codec_data_bytes) tuples.
    """
    unpacked = msgpack.unpackb(wire_bytes)
    results = {"frames": [], "signals": []}

    if not isinstance(unpacked, dict):
        return {"error": "not a dict"}

    if FIELD_FRAMES in unpacked:
        frames = unpacked[FIELD_FRAMES]
        if not isinstance(frames, list):
            frames = [frames]
        for frame in frames:
            codec_type_byte = frame[0]
            codec_data = frame[1:]
            results["frames"].append((codec_type_byte, bytes(codec_data)))

    if FIELD_SIGNALLING in unpacked:
        signals = unpacked[FIELD_SIGNALLING]
        if not isinstance(signals, list):
            signals = [signals]
        results["signals"] = signals

    return results


def snr_db(original, reconstructed):
    """Calculate Signal-to-Noise Ratio in dB between original and reconstructed signals."""
    # Align lengths
    min_len = min(len(original), len(reconstructed))
    orig = original[:min_len].astype(np.float64)
    recon = reconstructed[:min_len].astype(np.float64)

    signal_power = np.mean(orig ** 2)
    noise_power = np.mean((orig - recon) ** 2)

    if noise_power == 0:
        return float("inf")
    if signal_power == 0:
        return 0.0

    return 10 * math.log10(signal_power / noise_power)
