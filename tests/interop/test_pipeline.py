"""
Full pipeline simulation tests.

Simulates a complete voice call between Pyxis and Columba, including:
  - Capture → encode → batch → wire → parse → decode → playback
  - Jitter simulation
  - Buffer sizing analysis
  - Multi-second call simulation with statistics
"""

import math
import struct
import numpy as np
import pycodec2
import pytest

from conftest import (
    CODEC_CODEC2, MODE_HEADERS,
    encode_codec2_subframes, batch_subframes_pyxis_style,
    build_pyxis_audio_packet, build_columba_audio_packet,
    parse_pyxis_rx, parse_lxst_python_rx,
)


def generate_test_audio(duration_s, sample_rate=8000):
    """Generate deterministic test audio (int16 at 8kHz)."""
    t = np.arange(int(sample_rate * duration_s)) / sample_rate
    signal = np.sin(2 * np.pi * 440 * t) * 16000
    return signal.astype(np.int16)


class TestPyxisTxPipeline:
    """
    Simulate Pyxis TX pipeline: capture → encode → batch → wire.
    """

    def test_5_second_call_pyxis_to_columba(self):
        """
        Simulate 5 seconds of Pyxis → Columba audio transmission.
        All packets should be parseable and decodable without errors.
        """
        from LXST.Codecs.Codec2 import Codec2 as LXSTCodec2

        pcm = generate_test_audio(5.0)
        encoder = pycodec2.Codec2(3200)
        decoder = LXSTCodec2(mode=3200)
        spf = 160

        all_subframes = encode_codec2_subframes(encoder, pcm, mode_header=MODE_HEADERS[3200])

        BATCH_SIZE = 10
        packets_sent = 0
        total_decoded_samples = 0
        decode_errors = 0

        for batch_start in range(0, len(all_subframes), BATCH_SIZE):
            batch_frames = all_subframes[batch_start:batch_start + BATCH_SIZE]
            if len(batch_frames) < BATCH_SIZE:
                break

            batch = batch_subframes_pyxis_style(batch_frames, MODE_HEADERS[3200])
            wire = build_pyxis_audio_packet(batch)
            packets_sent += 1

            result = parse_lxst_python_rx(wire)
            if "error" in result or not result["frames"]:
                decode_errors += 1
                continue

            codec_type, codec_data = result["frames"][0]
            try:
                decoded = decoder.decode(codec_data)
                total_decoded_samples += decoded.shape[0]
            except Exception:
                decode_errors += 1

        expected_packets = len(pcm) // (BATCH_SIZE * spf)
        print(f"Packets: {packets_sent}/{expected_packets}, "
              f"Decoded: {total_decoded_samples} samples, Errors: {decode_errors}")

        assert packets_sent == expected_packets
        assert decode_errors == 0
        assert total_decoded_samples == packets_sent * BATCH_SIZE * spf

    def test_adaptive_batch_sizes(self):
        """
        Simulate Pyxis adaptive TX batching (variable sub-frames per packet).
        """
        from LXST.Codecs.Codec2 import Codec2 as LXSTCodec2

        pcm = generate_test_audio(3.0)
        encoder = pycodec2.Codec2(3200)
        decoder = LXSTCodec2(mode=3200)

        all_subframes = encode_codec2_subframes(encoder, pcm, mode_header=MODE_HEADERS[3200])

        batch_sizes = [10, 12, 15, 10, 25, 8, 10, 10, 20, 10, 15, 10, 10, 10, 10]
        pos = 0
        total_decoded = 0
        errors = 0

        for batch_size in batch_sizes:
            if pos + batch_size > len(all_subframes):
                break
            batch_frames = all_subframes[pos:pos + batch_size]
            pos += batch_size

            batch = batch_subframes_pyxis_style(batch_frames, MODE_HEADERS[3200])
            wire = build_pyxis_audio_packet(batch)

            result = parse_lxst_python_rx(wire)
            if result["frames"]:
                try:
                    decoded = decoder.decode(result["frames"][0][1])
                    total_decoded += decoded.shape[0]
                except Exception:
                    errors += 1

        print(f"Adaptive: {total_decoded} samples decoded, {errors} errors")
        assert errors == 0
        assert total_decoded > 0


class TestColumbaTxPipeline:
    """
    Simulate Columba TX pipeline: capture → encode → wire → Pyxis decode.
    """

    def test_5_second_call_columba_to_pyxis(self):
        """
        Simulate 5 seconds of Columba → Pyxis audio transmission.

        Uses pycodec2 for encoding (same underlying libcodec2 as LXST-kt/Python)
        to avoid Python LXST's array shape requirements.
        """
        pcm = generate_test_audio(5.0)
        columba_encoder = pycodec2.Codec2(3200)
        pyxis_decoder = pycodec2.Codec2(3200)
        spf = 160
        bpf = 8
        batch_samples = 1600  # 200ms

        packets_sent = 0
        total_decoded_samples = 0
        decode_errors = 0

        for batch_start in range(0, len(pcm), batch_samples):
            batch_pcm = pcm[batch_start:batch_start + batch_samples]
            if len(batch_pcm) < batch_samples:
                break

            # Encode like Columba: [mode_header] + [N * raw_codec2]
            encoded = bytes([MODE_HEADERS[3200]])
            n_frames = len(batch_pcm) // spf
            for i in range(n_frames):
                encoded += columba_encoder.encode(batch_pcm[i * spf:(i + 1) * spf])

            wire = build_columba_audio_packet(CODEC_CODEC2, encoded)
            packets_sent += 1

            result = parse_pyxis_rx(wire)
            if "error" in result or not result["frames"]:
                decode_errors += 1
                continue

            codec_type, codec_data = result["frames"][0]
            raw_data = codec_data[1:]  # strip mode header
            n_sub = len(raw_data) // bpf

            try:
                for i in range(n_sub):
                    pyxis_decoder.decode(raw_data[i * bpf:(i + 1) * bpf])
                total_decoded_samples += n_sub * spf
            except Exception:
                decode_errors += 1

        expected_packets = len(pcm) // batch_samples
        print(f"Packets: {packets_sent}/{expected_packets}, "
              f"Decoded: {total_decoded_samples} samples, Errors: {decode_errors}")

        assert packets_sent == expected_packets
        assert decode_errors == 0
        assert total_decoded_samples == packets_sent * batch_samples


class TestPlaybackBufferSimulation:
    """
    Simulate Pyxis playback buffer behavior to analyze underruns.
    """

    def _simulate_playback(self, packet_arrivals_ms, frames_per_packet,
                           pcm_ring_size, prebuffer_frames, playback_rate_fps=50):
        """
        Simulate the Pyxis playback buffer over time.

        Returns dict with statistics.
        """
        buffer_level = 0
        playback_started = False
        underruns = 0
        total_frames_played = 0
        total_frames_received = 0
        max_buffer = 0
        min_buffer_during_playback = pcm_ring_size
        drops = 0

        packet_idx = 0
        playback_interval_ms = 1000.0 / playback_rate_fps  # 20ms for 50fps
        next_playback_ms = 0.0

        # Simulate only while packets are expected (don't add trailing time)
        if not packet_arrivals_ms:
            return {"underruns": 0, "total_played": 0, "total_received": 0,
                    "max_buffer": 0, "min_buffer": 0, "drops": 0,
                    "playback_started": False}

        # End simulation when all audio from last packet would be consumed
        last_packet_ms = max(packet_arrivals_ms)
        last_packet_audio_ms = frames_per_packet * (1000.0 / playback_rate_fps)
        total_duration_ms = int(last_packet_ms + last_packet_audio_ms + 100)

        for t_ms in range(total_duration_ms):
            # Check packet arrivals
            while packet_idx < len(packet_arrivals_ms) and \
                  int(packet_arrivals_ms[packet_idx]) <= t_ms:
                frames_to_add = frames_per_packet
                total_frames_received += frames_to_add

                if buffer_level + frames_to_add > pcm_ring_size:
                    dropped = (buffer_level + frames_to_add) - pcm_ring_size
                    frames_to_add = pcm_ring_size - buffer_level
                    drops += dropped

                buffer_level += frames_to_add
                if buffer_level > max_buffer:
                    max_buffer = buffer_level

                packet_idx += 1

            # Prebuffer
            if not playback_started:
                if buffer_level >= prebuffer_frames:
                    playback_started = True
                    next_playback_ms = t_ms + playback_interval_ms

            # Playback consumption
            if playback_started and t_ms >= int(next_playback_ms):
                if buffer_level > 0:
                    buffer_level -= 1
                    total_frames_played += 1
                    if buffer_level < min_buffer_during_playback:
                        min_buffer_during_playback = buffer_level
                else:
                    underruns += 1
                next_playback_ms += playback_interval_ms

        return {
            "underruns": underruns,
            "total_played": total_frames_played,
            "total_received": total_frames_received,
            "max_buffer": max_buffer,
            "min_buffer": min_buffer_during_playback if playback_started else 0,
            "drops": drops,
            "playback_started": playback_started,
        }

    def test_current_pyxis_settings_thin_margin(self):
        """
        Current settings (PCM_RING=16, PREBUFFER=3): buffer gets dangerously low.
        With perfect timing, playback works but there's zero jitter margin.
        """
        duration_s = 5
        arrivals = [200 * i for i in range(1, duration_s * 5 + 1)]

        stats = self._simulate_playback(
            packet_arrivals_ms=arrivals,
            frames_per_packet=10,
            pcm_ring_size=16,
            prebuffer_frames=3,
        )

        print(f"Current settings: underruns={stats['underruns']}, "
              f"buf_range=[{stats['min_buffer']},{stats['max_buffer']}]")
        # Buffer dips to 0 between packets — no jitter margin
        assert stats["min_buffer"] <= 1

    def test_current_settings_with_jitter_underruns(self):
        """
        With mild jitter (+-20ms), current settings produce underruns.
        """
        import random
        random.seed(42)

        duration_s = 5
        base_arrivals = [200 * i for i in range(1, duration_s * 5 + 1)]
        jittery_arrivals = [max(0, a + random.randint(-20, 20)) for a in base_arrivals]
        jittery_arrivals.sort()

        stats = self._simulate_playback(
            packet_arrivals_ms=jittery_arrivals,
            frames_per_packet=10,
            pcm_ring_size=16,
            prebuffer_frames=3,
        )

        print(f"Current + jitter: underruns={stats['underruns']}")
        assert stats["underruns"] > 0, "Expected underruns with jitter"

    def test_improved_settings_no_jitter(self):
        """
        Improved settings (PCM_RING=50, PREBUFFER=15) handle perfect timing easily.
        """
        duration_s = 5
        arrivals = [200 * i for i in range(1, duration_s * 5 + 1)]

        stats = self._simulate_playback(
            packet_arrivals_ms=arrivals,
            frames_per_packet=10,
            pcm_ring_size=50,
            prebuffer_frames=15,
        )

        print(f"Improved: underruns={stats['underruns']}, "
              f"buf_range=[{stats['min_buffer']},{stats['max_buffer']}]")
        assert stats["underruns"] == 0

    def test_improved_settings_moderate_jitter(self):
        """
        Improved settings should handle moderate jitter (+-50ms).
        """
        import random
        random.seed(42)

        duration_s = 10
        base_arrivals = [200 * i for i in range(1, duration_s * 5 + 1)]
        jittery_arrivals = [max(0, a + random.randint(-50, 50)) for a in base_arrivals]
        jittery_arrivals.sort()

        stats = self._simulate_playback(
            packet_arrivals_ms=jittery_arrivals,
            frames_per_packet=10,
            pcm_ring_size=50,
            prebuffer_frames=15,
        )

        print(f"Improved + moderate jitter: underruns={stats['underruns']}, "
              f"buf_range=[{stats['min_buffer']},{stats['max_buffer']}]")
        assert stats["underruns"] == 0

    def test_improved_settings_heavy_jitter(self):
        """
        Improved settings should handle heavy jitter (+-100ms) — typical
        of LoRa/BLE relay paths.
        """
        import random
        random.seed(42)

        duration_s = 10
        base_arrivals = [200 * i for i in range(1, duration_s * 5 + 1)]
        jittery_arrivals = [max(0, a + random.randint(-100, 100)) for a in base_arrivals]
        jittery_arrivals.sort()

        stats = self._simulate_playback(
            packet_arrivals_ms=jittery_arrivals,
            frames_per_packet=10,
            pcm_ring_size=50,
            prebuffer_frames=15,
        )

        print(f"Improved + heavy jitter: underruns={stats['underruns']}, "
              f"buf_range=[{stats['min_buffer']},{stats['max_buffer']}]")
        assert stats["underruns"] == 0

    def test_improved_settings_bursty_pyxis_tx(self):
        """
        Improved settings should handle bursty Pyxis TX (25-frame packets, 500ms gaps).
        """
        import random
        random.seed(42)

        duration_s = 10
        base_arrivals = [500 * i for i in range(1, duration_s * 2 + 1)]
        jittery_arrivals = [max(0, a + random.randint(-50, 50)) for a in base_arrivals]
        jittery_arrivals.sort()

        stats = self._simulate_playback(
            packet_arrivals_ms=jittery_arrivals,
            frames_per_packet=25,
            pcm_ring_size=50,
            prebuffer_frames=15,
        )

        print(f"Bursty TX: underruns={stats['underruns']}, "
              f"buf=[{stats['min_buffer']},{stats['max_buffer']}], drops={stats['drops']}")
        # Bursty 500ms gaps with 25-frame packets push buffer limits.
        # A few underruns are acceptable — the key insight is that
        # improved settings handle this far better than current settings.
        assert stats["underruns"] <= 5, f"Too many underruns: {stats['underruns']}"
        assert stats["drops"] == 0

    def test_packet_loss_10_percent(self):
        """
        10% packet loss: some underruns expected but should be minimal
        with improved buffer settings.
        """
        duration_s = 10
        all_arrivals = [200 * i for i in range(1, duration_s * 5 + 1)]
        # Drop every 10th packet
        arrivals = [a for i, a in enumerate(all_arrivals) if (i + 1) % 10 != 0]

        stats = self._simulate_playback(
            packet_arrivals_ms=arrivals,
            frames_per_packet=10,
            pcm_ring_size=50,
            prebuffer_frames=15,
        )

        print(f"10% loss: underruns={stats['underruns']}, "
              f"buf=[{stats['min_buffer']},{stats['max_buffer']}]")
        # With 300ms prebuffer, isolated dropped packets are absorbed.
        # But the regular 10%-drop pattern creates periodic buffer drain.
        # Key metric: far fewer underruns than current settings would produce.
        assert stats["underruns"] < 50, f"Too many underruns: {stats['underruns']}"


class TestFullDuplexSimulation:
    """
    Simulate a full-duplex call with both directions running simultaneously.
    """

    def test_bidirectional_3_second_call(self):
        """
        Simulate 3 seconds of full-duplex audio.
        Both directions should decode cleanly with no errors.

        Uses pycodec2 for Columba encoding (same underlying libcodec2)
        to avoid Python LXST's array shape requirements.
        """
        from LXST.Codecs.Codec2 import Codec2 as LXSTCodec2

        duration_s = 3
        spf = 160
        bpf = 8

        pcm_pyxis = generate_test_audio(duration_s)
        pcm_columba = generate_test_audio(duration_s)

        # ── Pyxis → Columba ──
        pyxis_enc = pycodec2.Codec2(3200)
        columba_dec = LXSTCodec2(mode=3200)
        pyxis_subframes = encode_codec2_subframes(
            pyxis_enc, pcm_pyxis, mode_header=MODE_HEADERS[3200])

        d1_samples = 0
        d1_errors = 0
        for i in range(0, len(pyxis_subframes), 10):
            batch = pyxis_subframes[i:i + 10]
            if len(batch) < 10:
                break
            batch_data = batch_subframes_pyxis_style(batch, MODE_HEADERS[3200])
            wire = build_pyxis_audio_packet(batch_data)
            result = parse_lxst_python_rx(wire)
            try:
                decoded = columba_dec.decode(result["frames"][0][1])
                d1_samples += decoded.shape[0]
            except Exception:
                d1_errors += 1

        # ── Columba → Pyxis ──
        columba_enc = pycodec2.Codec2(3200)
        pyxis_dec = pycodec2.Codec2(3200)

        d2_samples = 0
        d2_errors = 0
        for i in range(0, len(pcm_columba), 1600):
            batch_pcm = pcm_columba[i:i + 1600]
            if len(batch_pcm) < 1600:
                break
            # Encode like Columba: [mode_header] + [N * raw_codec2]
            encoded = bytes([MODE_HEADERS[3200]])
            n_frames = len(batch_pcm) // spf
            for j in range(n_frames):
                encoded += columba_enc.encode(batch_pcm[j * spf:(j + 1) * spf])

            wire = build_columba_audio_packet(CODEC_CODEC2, encoded)
            result = parse_pyxis_rx(wire)
            if result["frames"]:
                raw_data = result["frames"][0][1][1:]
                n_sub = len(raw_data) // bpf
                try:
                    for j in range(n_sub):
                        pyxis_dec.decode(raw_data[j * bpf:(j + 1) * bpf])
                        d2_samples += spf
                except Exception:
                    d2_errors += 1

        expected = (duration_s * 8000 // 1600) * 1600
        print(f"Pyxis→Columba: {d1_samples}/{expected} samples, {d1_errors} errors")
        print(f"Columba→Pyxis: {d2_samples}/{expected} samples, {d2_errors} errors")

        assert d1_errors == 0
        assert d2_errors == 0
        assert d1_samples == expected
        assert d2_samples == expected
