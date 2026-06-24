#!/usr/bin/env python3
"""
pyxis LXST voice-quality test harness (acoustic loopback).

Physical setup: the Mac's speaker sits right next to the T-Deck's microphone.
This harness plays a known reference signal out the Mac speaker; the T-Deck mic
captures it, runs the real pipeline (ES7210 -> filters/AGC -> Codec2 encode ->
call_send_audio_batch framing -> call_on_packet parse -> Codec2 decode), and dumps
the decoded PCM back over UDP multicast. We re-align and score the round-trip so we
can (a) confirm a profile produces intelligible audio and (b) compare profiles and
before/after a firmware change.

Firmware contract (T:LOOPBACK test mode):
  - UDP multicast 239.0.99.99:9998, each datagram = [uint32 LE byte_offset][int16 LE
    mono PCM @ 8 kHz], payload <= 1284 bytes.
  - Serial T: hooks: `T:CALL_PROFILE 0x10|0x20|0x30` (ULBW/VLBW/LBW),
    `T:LOOPBACK on`, `T:LOOPBACK off`.

Usage:
  python3 run_voice_test.py --port /dev/cu.usbmodem101 --profiles ULBW,VLBW,LBW
  python3 run_voice_test.py --port /dev/cu.usbmodem101 --profiles LBW --ref my.wav

Deps:  pip install numpy scipy soundfile sounddevice pyserial    (optional: pystoi)
Make sure the Mac OUTPUT device is the built-in speaker next to the T-Deck, and
turn the volume to a consistent, moderate level.
"""

import argparse, socket, struct, sys, time, threading, subprocess, os

SR = 8000                         # Codec2 / pipeline sample rate
MCAST_GRP, MCAST_PORT = "239.0.99.99", 9998
PROFILES = {"ULBW": 0x10, "VLBW": 0x20, "LBW": 0x30}   # 700C / 1600 / 3200
OUTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")


def _need(mods):
    miss = []
    for m in mods:
        try:
            __import__(m)
        except ImportError:
            miss.append(m)
    if miss:
        sys.exit(f"Missing deps: {', '.join(miss)}\n  pip install {' '.join(miss)}")


_need(["numpy", "scipy", "soundfile", "sounddevice", "serial"])
import numpy as np
from scipy import signal as sps
import soundfile as sf
import sounddevice as sd
import serial

try:
    from pystoi import stoi as _stoi
    HAVE_STOI = True
except Exception:
    HAVE_STOI = False


# ---------------- serial T: hooks ----------------
class Dev:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=1.0)
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def cmd(self, line, wait=0.4):
        self.s.write((line + "\n").encode())
        self.s.flush()
        time.sleep(wait)
        out = b""
        while self.s.in_waiting:
            out += self.s.read(self.s.in_waiting)
            time.sleep(0.05)
        return out.decode("utf-8", "replace")

    def close(self):
        try:
            self.cmd("T:LOOPBACK off")
        except Exception:
            pass
        self.s.close()


# ---------------- UDP multicast receiver ----------------
class PcmReceiver(threading.Thread):
    """Collects [uint32 offset][int16 PCM] datagrams into a contiguous buffer."""

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", MCAST_PORT))
        mreq = struct.pack("=4sl", socket.inet_aton(MCAST_GRP), socket.INADDR_ANY)
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        self.sock.settimeout(0.5)
        self.chunks = {}           # offset -> bytes
        self.max_end = 0
        self.packets = 0
        self._run = True

    def run(self):
        while self._run:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 4:
                continue
            off = struct.unpack("<I", data[:4])[0]
            pcm = data[4:]
            self.chunks[off] = pcm
            self.packets += 1
            self.max_end = max(self.max_end, off + len(pcm))

    def stop(self):
        self._run = False
        try:
            self.sock.close()
        except Exception:
            pass

    def assemble(self):
        """Return int16 PCM with gaps zero-filled, plus a dropped-byte count."""
        buf = bytearray(self.max_end)
        filled = bytearray(self.max_end)
        for off, pcm in self.chunks.items():
            end = min(off + len(pcm), self.max_end)
            buf[off:end] = pcm[: end - off]
            for i in range(off, end):
                filled[i] = 1
        dropped = self.max_end - sum(filled)
        pcm = np.frombuffer(bytes(buf), dtype="<i2").astype(np.float32) / 32768.0
        return pcm, dropped


# ---------------- reference signal ----------------
def build_reference(speech_text):
    """[1s log chirp 200-3400 Hz][0.4s silence][speech] at 8 kHz, float32 in [-1,1].

    The chirp gives a sharp cross-correlation peak for alignment + a frequency
    sweep to see which bands survive. Speech (via macOS `say`) drives intelligibility.
    """
    t = np.linspace(0, 1.0, SR, endpoint=False)
    chirp = 0.6 * sps.chirp(t, f0=200, f1=3400, t1=1.0, method="logarithmic").astype(np.float32)
    chirp *= np.hanning(len(chirp)).astype(np.float32) ** 0.25
    gap = np.zeros(int(0.4 * SR), np.float32)

    speech = np.zeros(0, np.float32)
    try:
        os.makedirs(OUTDIR, exist_ok=True)
        aiff = os.path.join(OUTDIR, "_say.aiff")
        subprocess.run(["say", "-o", aiff, speech_text], check=True)
        sp, sr0 = sf.read(aiff)
        if sp.ndim > 1:
            sp = sp.mean(axis=1)
        if sr0 != SR:
            sp = sps.resample(sp, int(len(sp) * SR / sr0))
        sp = sp.astype(np.float32)
        sp = 0.7 * sp / (np.max(np.abs(sp)) + 1e-9)
        speech = sp
    except Exception as e:
        print(f"  (say unavailable: {e}; using tone triplet instead)")
        tt = np.linspace(0, 1.5, int(1.5 * SR), endpoint=False)
        speech = 0.5 * (np.sin(2 * np.pi * 500 * tt) + np.sin(2 * np.pi * 1200 * tt)
                        + np.sin(2 * np.pi * 2400 * tt)).astype(np.float32) / 3

    ref = np.concatenate([chirp, gap, speech]).astype(np.float32)
    return ref, len(chirp)


# ---------------- scoring ----------------
def _env(x):
    """Smoothed amplitude envelope (~20 Hz). Codec2 is a vocoder — the waveform is
    NOT preserved, but the energy envelope is, so we align/score on the envelope."""
    e = np.abs(sps.hilbert(x))
    b, a = sps.butter(2, 20 / (SR / 2))
    return sps.filtfilt(b, a, e).astype(np.float32)


def align(ref, cap):
    """Align via energy-envelope cross-correlation (vocoder-safe)."""
    er, ec = _env(ref), _env(cap)
    xc = sps.correlate(ec, er, mode="full")
    lag = int(np.argmax(xc) - (len(er) - 1))
    if lag < 0:
        lag = 0
    cap = cap[lag:]
    m = min(len(ref), len(cap))
    return ref[:m], cap[:m], lag


def band_energy(x, lo, hi):
    f, p = sps.welch(x, SR, nperseg=min(1024, len(x)))
    m = (f >= lo) & (f <= hi)
    return float(np.sum(p[m]))


def score(ref_full, cap_full, chirp_len):
    """Return a dict of metrics, scoring on the SPEECH part (after the chirp+gap)."""
    res = {}
    if len(cap_full) < SR // 2:
        res["error"] = f"no/short audio captured ({len(cap_full)} samples)"
        return res, ref_full, cap_full
    ref, cap, lag = align(ref_full, cap_full)
    res["lag_ms"] = round(lag * 1000 / SR, 1)
    if len(cap) < SR:
        res["error"] = "captured audio too short to score"
        return res, ref, cap

    # Score the speech region (skip chirp + gap)
    start = chirp_len + int(0.4 * SR)
    r = ref[start:] if len(ref) > start else ref
    c = cap[start:] if len(cap) > start else cap
    m = min(len(r), len(c))
    r, c = r[:m], c[:m]
    if m < SR // 2:
        res["error"] = "speech region too short"
        return res, ref, cap

    # env_conf: does the capture actually CONTAIN the reference? (envelope corr after
    # alignment). Low -> the mic isn't hearing the playback; the rest is then moot.
    res["env_conf"] = round(float(np.corrcoef(_env(r), _env(c))[0, 1]), 2)

    # spectral-envelope correlation over time (vocoder-appropriate fidelity proxy)
    _, _, Sr = sps.spectrogram(r, SR, nperseg=256, noverlap=128)
    _, _, Sc = sps.spectrogram(c, SR, nperseg=256, noverlap=128)
    mm = min(Sr.shape[1], Sc.shape[1])
    Lr, Lc = np.log(Sr[:, :mm] + 1e-9), np.log(Sc[:, :mm] + 1e-9)
    res["spec_corr"] = round(float(np.corrcoef(Lr.flatten(), Lc.flatten())[0, 1]), 3)

    # speech-band survival (energy-normalized)
    g = np.sqrt((np.dot(r, r) + 1e-9) / (np.dot(c, c) + 1e-9))
    res["band"] = round((band_energy(c * g, 300, 3400) + 1e-12) / (band_energy(r, 300, 3400) + 1e-12), 2)

    if HAVE_STOI:
        try:
            res["stoi"] = round(float(_stoi(r, c, SR, extended=False)), 3)
        except Exception:
            res["stoi"] = "err"
    return res, ref, cap


def verdict(res):
    if "error" in res:
        return "FAIL (" + res["error"] + ")"
    if res.get("env_conf", 0) < 0.3:
        return "NO-CAPTURE (mic not hearing playback)"
    s = res.get("stoi")
    if isinstance(s, float):
        return "INTELLIGIBLE" if s >= 0.55 else ("MARGINAL" if s >= 0.45 else "POOR")
    return "?"


# ---------------- one profile run ----------------
def run_profile(dev, name, ref, chirp_len, tail_s):
    print(f"\n=== profile {name} (0x{PROFILES[name]:02X}) ===")
    print("  set profile:", dev.cmd(f"T:CALL_PROFILE 0x{PROFILES[name]:02X}").strip())
    rx = PcmReceiver(); rx.start()
    time.sleep(0.2)
    print("  loopback on:", dev.cmd("T:LOOPBACK on").strip())
    time.sleep(0.6)                      # let capture/playback spin up
    sd.play(ref, SR); sd.wait()          # play reference out the Mac speaker
    time.sleep(tail_s)                   # drain pipeline latency (~1 s)
    dev.cmd("T:LOOPBACK off")
    time.sleep(0.3); rx.stop(); rx.join(timeout=1)

    cap, dropped = rx.assemble()
    os.makedirs(OUTDIR, exist_ok=True)
    sf.write(os.path.join(OUTDIR, f"captured_{name}.wav"), cap, SR)
    res, refA, capA = score(ref, cap, chirp_len)
    res["packets"] = rx.packets
    res["cap_sec"] = round(len(cap) / SR, 2)
    res["dropped_bytes"] = dropped
    print(f"  packets={rx.packets} cap={res['cap_sec']}s dropped={dropped}B lag={res.get('lag_ms','?')}ms")
    print(f"  -> {verdict(res)}  {{ " +
          ", ".join(f"{k}={res[k]}" for k in
                    ("stoi", "env_conf", "spec_corr", "band", "lag_ms") if k in res) + " }")
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem101")
    ap.add_argument("--profiles", default="ULBW,VLBW,LBW")
    ap.add_argument("--ref", help="reference WAV (else generated chirp+speech)")
    ap.add_argument("--text", default="The quick brown fox jumps over the lazy dog. "
                    "She sells seashells. Numbers: one two three four five.")
    ap.add_argument("--tail", type=float, default=1.2, help="post-play drain seconds")
    ap.add_argument("--output", default="speaker",
                    help="output device name substring (the Mac speaker next to the T-Deck mic)")
    args = ap.parse_args()

    # Select the output device so audio plays out the speaker (not a monitor/HDMI).
    out_dev = next((i for i, d in enumerate(sd.query_devices())
                    if d["max_output_channels"] > 0 and args.output.lower() in d["name"].lower()), None)
    if out_dev is not None:
        sd.default.device = (None, out_dev)
        print(f"output device: {sd.query_devices(out_dev)['name']}")
    else:
        print(f"output device: system default (no '{args.output}' match)")

    os.makedirs(OUTDIR, exist_ok=True)
    if args.ref:
        ref, sr0 = sf.read(args.ref)
        if ref.ndim > 1:
            ref = ref.mean(axis=1)
        if sr0 != SR:
            ref = sps.resample(ref, int(len(ref) * SR / sr0))
        ref = (0.7 * ref / (np.max(np.abs(ref)) + 1e-9)).astype(np.float32)
        chirp_len = 0
    else:
        ref, chirp_len = build_reference(args.text)
    sf.write(os.path.join(OUTDIR, "reference.wav"), ref, SR)
    print(f"reference: {len(ref)/SR:.2f}s  STOI={'yes' if HAVE_STOI else 'no (pip install pystoi)'}  out={OUTDIR}")

    dev = Dev(args.port)
    results = {}
    try:
        # The mic-capture FreeRTOS task needs a contiguous internal-heap stack;
        # BLE holds enough heap to make that allocation fail intermittently. Free it
        # for the duration of the test, restore after.
        print("BLE off (free heap):", dev.cmd("T:BLE off", wait=1.8).strip().splitlines()[-1:])
        for name in [p.strip().upper() for p in args.profiles.split(",") if p.strip()]:
            if name not in PROFILES:
                print(f"  skip unknown profile {name}"); continue
            results[name] = run_profile(dev, name, ref, chirp_len, args.tail)
    finally:
        try:
            dev.cmd("T:BLE on")
        except Exception:
            pass
        dev.close()

    print("\n================ SUMMARY ================")
    hdr = f"{'profile':7} {'verdict':33} {'stoi':6} {'envConf':7} {'specCorr':8} {'band':5} {'pkts':5}"
    print(hdr); print("-" * len(hdr))
    for name, r in results.items():
        print(f"{name:7} {verdict(r):33} {str(r.get('stoi','-')):6} {str(r.get('env_conf','-')):7} "
              f"{str(r.get('spec_corr','-')):8} {str(r.get('band','-')):5} {str(r.get('packets','-')):5}")
    print(f"\nWAVs in {OUTDIR}/ (reference.wav, captured_<profile>.wav) — listen to confirm.")


if __name__ == "__main__":
    main()
