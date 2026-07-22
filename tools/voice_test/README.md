# pyxis voice-quality test harness (acoustic loopback)

Automated end-to-end test of the LXST voice pipeline using the **Mac speaker next to
the T-Deck mic**. The Mac plays a known signal; the T-Deck captures it through the real
pipeline (ES7210 mic → filters/AGC → Codec2 encode → `call_send_audio_batch` framing →
`call_on_packet` parse → Codec2 decode) and streams the decoded PCM back over UDP. The
harness re-aligns and scores the round-trip.

## Physical setup
- Mac built-in **speaker** within a few cm of the T-Deck **mic**, quiet room.
- T-Deck on USB (serial control) **and** WiFi (UDP audio) — same LAN as the Mac.
- Set the Mac output volume to a consistent moderate level.

## Firmware contract (`T:LOOPBACK` test mode)
- UDP multicast `239.0.99.99:9998`, each datagram = `[uint32 LE byte-offset][int16 LE
  mono @ 8 kHz PCM]`, ≤1284 B.
- Serial hooks: `T:CALL_PROFILE 0x10|0x20|0x30` (ULBW/VLBW/LBW), `T:LOOPBACK on|off`.

## Run
```bash
cd tools/voice_test
.venv/bin/python run_voice_test.py --port /dev/cu.usbmodem101 --profiles ULBW,VLBW,LBW
# options: --ref my.wav   --text "..."   --output "Mac mini Speakers"   --tail 1.2
```

Outputs `out/reference.wav` + `out/captured_<profile>.wav` (listen to confirm) and a
summary table.

## Metrics
- **STOI** (0–1): objective intelligibility (≥0.55 ≈ intelligible). Primary score.
- **corr**: time-aligned, gain-matched waveform correlation of the speech region.
- **segSNR (dB)**: segmental SNR after alignment/gain-match.
- **band**: speech-band (300–3400 Hz) energy survival vs reference.
- **hf**: high-band (2–3.4 kHz) survival — Codec2 low profiles roll off the top.
- **pkts / dropped**: UDP delivery health.

Lower-bitrate profiles (700C) score lower by design; compare profiles and before/after
firmware changes. The venv was created with `uv venv --python 3.12` +
`numpy scipy soundfile sounddevice pyserial pystoi`.

## Sideband/LXST end-to-end regression

`sideband_e2e.py` uses the Mac's real RNS/LXST/Sideband Python stack and the
T-Deck serial hooks to verify:

- Pyxis calls Sideband and exchanges synthetic Codec2 audio in both directions.
- Sideband calls Pyxis and exchanges synthetic Codec2 audio in both directions.
- Pyxis still accepts another incoming call after both calls and hangups.
- Every Pyxis decode succeeds and produces non-zero PCM.

Build the test firmware with the Mac TCP server baked in, then upload it:

```bash
export PYXIS_TEST_TCP_HOST=10.0.0.145 PYXIS_TEST_TCP_PORT=4242
/opt/homebrew/bin/pio run -e tdeck -t upload --upload-port /dev/cu.usbmodem101
```

Run from the Mac with its Reticulum venv (defaults to the local TCP server on
`127.0.0.1:4242`):

```bash
~/.reticulum-host/venv/bin/python tools/voice_test/sideband_e2e.py
```

Optional overrides: `PYXIS_SERIAL_PORT`, `PYXIS_RNS_HOST`,
`PYXIS_RNS_PORT`, and `PYXIS_CALL_SECONDS`. Each run uses a fresh Sideband
identity and isolated RNS storage so stale cached paths cannot false-pass setup.
On Apple Silicon the harness re-executes itself with `/opt/homebrew/lib` on
`DYLD_LIBRARY_PATH`, allowing LXST/PyOgg to load Homebrew `libopus` for incoming
calls.
