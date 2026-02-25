"""
Wire format interop tests.

Verifies that packets constructed by Pyxis can be parsed by Python LXST
(and vice versa), ensuring msgpack encoding/decoding compatibility.

Tests cover:
  - Single audio frame packets (bin8)
  - Batched audio frame packets (Pyxis TX style)
  - Signalling packets (fixint, uint8, uint16)
  - Profile negotiation signals
  - Edge cases (empty, max size, etc.)
"""

import struct
import numpy as np
import pycodec2
import pytest

from conftest import (
    CODEC_CODEC2, CODEC_OPUS, FIELD_FRAMES, FIELD_SIGNALLING,
    MODE_HEADERS, HEADER_MODES,
    STATUS_AVAILABLE, STATUS_RINGING, STATUS_ESTABLISHED,
    PREFERRED_PROFILE, PROFILE_LBW, PROFILE_MQ,
    build_pyxis_audio_packet, build_columba_audio_packet,
    build_signal_packet, parse_pyxis_rx, parse_lxst_python_rx,
    encode_codec2_subframes, batch_subframes_pyxis_style,
)

from RNS.vendor import umsgpack as msgpack


class TestPyxisTxFormat:
    """Test that Pyxis-format packets can be parsed by Python LXST."""

    def test_single_subframe_packet(self, codec2_3200, sine_8khz_200ms):
        """A single 20ms Codec2 sub-frame as bin8 — simplest case."""
        spf = codec2_3200.samples_per_frame()  # 160
        pcm = sine_8khz_200ms[:spf]  # first 160 samples
        encoded = codec2_3200.encode(pcm)  # 8 bytes raw

        # Build batch with 1 frame: [mode_header(0x06)] + [8 bytes]
        batch = bytes([MODE_HEADERS[3200]]) + encoded
        wire = build_pyxis_audio_packet(batch)

        # Parse with Python LXST logic
        result = parse_lxst_python_rx(wire)
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2
        assert codec_data[0] == MODE_HEADERS[3200]  # mode header
        assert len(codec_data) == 1 + 8  # mode_header + 1 sub-frame

    def test_10_subframe_batch_lbw(self, codec2_3200, sine_8khz_200ms):
        """
        10 sub-frames batched (200ms = 1 LXST LBW "frame").
        This is the most common Pyxis TX packet size.
        """
        subframes = encode_codec2_subframes(codec2_3200, sine_8khz_200ms[:1600])
        assert len(subframes) == 10

        batch = batch_subframes_pyxis_style(subframes, MODE_HEADERS[3200])
        wire = build_pyxis_audio_packet(batch)

        # Parse with Python LXST
        result = parse_lxst_python_rx(wire)
        assert len(result["frames"]) == 1  # single bin8, not array
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2

        # Verify codec_data structure: [mode_header] + [10 * 8 bytes]
        assert codec_data[0] == MODE_HEADERS[3200]
        assert len(codec_data) == 1 + 10 * 8  # 81 bytes

    def test_25_subframe_batch(self, codec2_3200, sine_8khz_1s):
        """25 sub-frames batched (500ms) — Pyxis adaptive batching max."""
        subframes = encode_codec2_subframes(codec2_3200, sine_8khz_1s[:4000])
        assert len(subframes) == 25

        batch = batch_subframes_pyxis_style(subframes, MODE_HEADERS[3200])
        wire = build_pyxis_audio_packet(batch)

        result = parse_lxst_python_rx(wire)
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2
        assert len(codec_data) == 1 + 25 * 8  # 201 bytes

    def test_pyxis_packet_also_parseable_by_pyxis_rx(self, codec2_3200, sine_8khz_200ms):
        """Pyxis packets should also be parseable by our own RX parser."""
        subframes = encode_codec2_subframes(codec2_3200, sine_8khz_200ms[:1600])
        batch = batch_subframes_pyxis_style(subframes, MODE_HEADERS[3200])
        wire = build_pyxis_audio_packet(batch)

        result = parse_pyxis_rx(wire)
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2
        assert len(codec_data) == 1 + 10 * 8

    def test_lxst_python_can_decode_pyxis_batch(self, codec2_3200, lxst_codec2_3200, sine_8khz_200ms):
        """
        Full decode test: Python LXST Codec2 can decode a Pyxis batched packet.

        This is THE critical interop test — it verifies that the codec data
        extracted from a Pyxis wire packet can be decoded by Python LXST's
        Codec2.decode() method.
        """
        # Encode 10 sub-frames with pycodec2
        pcm = sine_8khz_200ms[:1600]
        subframes = encode_codec2_subframes(codec2_3200, pcm)
        batch = batch_subframes_pyxis_style(subframes, MODE_HEADERS[3200])

        # Build wire packet and parse
        wire = build_pyxis_audio_packet(batch)
        result = parse_lxst_python_rx(wire)
        codec_type, codec_data = result["frames"][0]

        # Decode with Python LXST Codec2
        # codec_data = [mode_header] + [10 * 8 bytes raw codec2]
        decoded = lxst_codec2_3200.decode(codec_data)
        assert decoded.shape[0] == 1600  # 10 * 160 samples
        assert decoded.shape[1] == 1     # mono

        # Verify audio is not silence
        max_amp = np.max(np.abs(decoded))
        assert max_amp > 0.01, f"Decoded audio is near-silence: max_amp={max_amp}"


class TestColumbaTxFormat:
    """Test that Columba/LXST-kt format packets can be parsed by Pyxis."""

    def test_single_lbw_frame(self, codec2_3200, sine_8khz_200ms):
        """
        Columba LBW: 200ms = 1600 samples → Codec2 encode → 81 bytes.
        Wire: {0x01: bin8([0x02, 0x06, 80 bytes])} = 86 bytes total.
        """
        pcm = sine_8khz_200ms[:1600]

        # Encode like LXST-kt Codec2.encode(): all 1600 samples at once
        spf = codec2_3200.samples_per_frame()
        bpf = codec2_3200.bytes_per_frame()
        n_frames = len(pcm) // spf
        encoded = bytes([MODE_HEADERS[3200]])
        for i in range(n_frames):
            chunk = pcm[i * spf:(i + 1) * spf]
            encoded += codec2_3200.encode(chunk)

        assert len(encoded) == 1 + 10 * 8  # 81 bytes

        # Build Columba wire packet
        wire = build_columba_audio_packet(CODEC_CODEC2, encoded)

        # Parse with Pyxis RX
        result = parse_pyxis_rx(wire)
        assert "error" not in result
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2
        assert codec_data[0] == MODE_HEADERS[3200]
        assert len(codec_data) == 81

    def test_columba_packet_decoded_by_pyxis_codec(self, codec2_3200, sine_8khz_200ms):
        """
        Full decode: Pyxis Codec2 decode logic can handle Columba's frame.

        Simulates what i2s_playback.cpp writeEncodedPacket() does:
        calls codec_->decode(codec_data, len) which reads mode header,
        then decodes N sub-frames.
        """
        pcm = sine_8khz_200ms[:1600]
        spf = codec2_3200.samples_per_frame()
        bpf = codec2_3200.bytes_per_frame()
        n_frames = len(pcm) // spf

        # Encode (Columba style)
        mode_header = bytes([MODE_HEADERS[3200]])
        raw_encoded = b""
        for i in range(n_frames):
            chunk = pcm[i * spf:(i + 1) * spf]
            raw_encoded += codec2_3200.encode(chunk)
        encoded = mode_header + raw_encoded

        # Build wire packet and parse like Pyxis
        wire = build_columba_audio_packet(CODEC_CODEC2, encoded)
        result = parse_pyxis_rx(wire)
        codec_type, codec_data = result["frames"][0]

        # Decode like Pyxis codec_wrapper.cpp:
        # codec_data[0] = mode header, codec_data[1:] = raw sub-frames
        header = codec_data[0]
        assert header == MODE_HEADERS[3200]
        raw_data = codec_data[1:]
        assert len(raw_data) == n_frames * bpf

        # Decode each sub-frame (simulating Codec2Wrapper::decode)
        # Use a fresh codec2 instance (simulating the decoder on Pyxis)
        decoder = pycodec2.Codec2(3200)
        decoded_pcm = np.zeros(n_frames * spf, dtype=np.int16)
        for i in range(n_frames):
            frame_data = raw_data[i * bpf:(i + 1) * bpf]
            decoded = decoder.decode(frame_data)
            decoded_pcm[i * spf:(i + 1) * spf] = decoded

        # Verify non-silence
        assert np.max(np.abs(decoded_pcm)) > 100

    def test_columba_also_parseable_by_lxst_python(self, codec2_3200, sine_8khz_200ms):
        """Columba packets should also be parseable by Python LXST (self-interop)."""
        pcm = sine_8khz_200ms[:1600]
        spf = codec2_3200.samples_per_frame()
        n_frames = len(pcm) // spf

        encoded = bytes([MODE_HEADERS[3200]])
        for i in range(n_frames):
            chunk = pcm[i * spf:(i + 1) * spf]
            encoded += codec2_3200.encode(chunk)

        wire = build_columba_audio_packet(CODEC_CODEC2, encoded)
        result = parse_lxst_python_rx(wire)
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2


class TestSignallingFormat:
    """Test signalling packet format compatibility."""

    def test_fixint_signal(self):
        """Signals 0x00-0x06 use fixint encoding (single byte)."""
        for signal in [STATUS_AVAILABLE, STATUS_RINGING, STATUS_ESTABLISHED]:
            wire = build_signal_packet(signal)

            # Both parsers should decode
            pyxis_result = parse_pyxis_rx(wire)
            lxst_result = parse_lxst_python_rx(wire)

            assert pyxis_result["signals"] == [signal]
            assert lxst_result["signals"] == [signal]

    def test_uint8_signal(self):
        """Signals 128-255 use uint8 encoding (0xCC XX)."""
        signal = PREFERRED_PROFILE  # 0xFF
        wire = build_signal_packet(signal)

        pyxis_result = parse_pyxis_rx(wire)
        lxst_result = parse_lxst_python_rx(wire)

        assert pyxis_result["signals"] == [signal]
        assert lxst_result["signals"] == [signal]

    def test_profile_negotiation_signal(self):
        """PREFERRED_PROFILE + profile_id uses uint16 encoding (0xCD XX XX)."""
        signal = PREFERRED_PROFILE + PROFILE_LBW  # 0xFF + 0x30 = 0x12F (303)
        wire = build_signal_packet(signal)

        pyxis_result = parse_pyxis_rx(wire)
        lxst_result = parse_lxst_python_rx(wire)

        assert pyxis_result["signals"] == [signal]
        assert lxst_result["signals"] == [signal]

    def test_pyxis_manual_signal_construction(self):
        """
        Verify Pyxis call_send_signal() manual msgpack matches standard msgpack.

        Pyxis constructs signal packets by hand (not using a msgpack library):
          fixint: [0x81, 0x00, 0x91, signal]
          uint8:  [0x81, 0x00, 0x91, 0xCC, signal]
          uint16: [0x81, 0x00, 0x91, 0xCD, hi, lo]
        """
        # fixint range (0-127)
        for signal in [0x00, 0x06, 0x7F]:
            manual = bytes([0x81, 0x00, 0x91, signal])
            standard = build_signal_packet(signal)
            assert manual == standard, f"Mismatch for fixint signal 0x{signal:02x}"

        # uint8 range (128-255) — only 0xFF used in practice
        signal = 0xFF
        manual = bytes([0x81, 0x00, 0x91, 0xCC, signal])
        standard = build_signal_packet(signal)
        assert manual == standard

        # uint16 range (256+) — profile negotiation
        signal = 0x12F  # PREFERRED_PROFILE + LBW
        hi = (signal >> 8) & 0xFF
        lo = signal & 0xFF
        manual = bytes([0x81, 0x00, 0x91, 0xCD, hi, lo])
        standard = build_signal_packet(signal)
        assert manual == standard


class TestWireFormatEdgeCases:
    """Edge cases and boundary conditions."""

    def test_max_bin8_payload(self, codec2_3200, sine_8khz_1s):
        """Test maximum bin8 payload (255 bytes)."""
        # 30 sub-frames: [mode(1)] + [30*8] = 241 bytes + codec_type(1) = 242 bytes
        subframes = encode_codec2_subframes(codec2_3200, sine_8khz_1s[:4800])
        batch = batch_subframes_pyxis_style(subframes[:30], MODE_HEADERS[3200])
        wire = build_pyxis_audio_packet(batch)

        result = parse_lxst_python_rx(wire)
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2
        # mode_header + 30 * 8 bytes = 241 bytes
        assert len(codec_data) == 1 + 30 * 8

        # Also parseable by Pyxis
        result2 = parse_pyxis_rx(wire)
        assert len(result2["frames"]) == 1

    def test_codec2_1600_mode(self, codec2_1600, sine_8khz_200ms):
        """Test VLBW profile with Codec2 1600 bps."""
        spf = codec2_1600.samples_per_frame()
        bpf = codec2_1600.bytes_per_frame()
        pcm = sine_8khz_200ms[:spf * 8]  # 8 sub-frames

        subframes = encode_codec2_subframes(codec2_1600, pcm)
        batch = batch_subframes_pyxis_style(subframes, MODE_HEADERS[1600])
        wire = build_pyxis_audio_packet(batch)

        result = parse_lxst_python_rx(wire)
        assert len(result["frames"]) == 1
        codec_type, codec_data = result["frames"][0]
        assert codec_type == CODEC_CODEC2
        assert codec_data[0] == MODE_HEADERS[1600]  # 0x04

    def test_raw_bytes_roundtrip(self):
        """Verify raw wire bytes match between manual and msgpack construction."""
        # Build a simple packet manually (as Pyxis does in call_send_audio_batch)
        batch_data = bytes([0x06]) + bytes(8)  # mode_header + 8 zero bytes
        codec_type = CODEC_CODEC2
        total_len = 1 + len(batch_data)  # codec_type + batch

        manual_wire = bytes([
            0x81,                   # fixmap(1)
            0x01,                   # key: FIELD_FRAMES
            0xC4,                   # bin8
            total_len,              # length
            codec_type,             # 0x02
        ]) + batch_data

        standard_wire = build_pyxis_audio_packet(batch_data)
        assert manual_wire == standard_wire
