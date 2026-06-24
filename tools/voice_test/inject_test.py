#!/usr/bin/env python3
"""Codec-isolation diagnostic: inject a known 1 kHz sine BEFORE the encoder
(T:CALL_INJECT, which replaces the mic in i2s_capture) and loop it back. If the
captured PCM is a clean ~1 kHz tone, the codec/framing/decode path is fine and the
mic is the culprit; if it's broadband garbage, the codec path itself is broken.

Resets the device first (the loopback teardown is currently flaky), waits for boot,
verifies responsiveness, then runs one clean injection.

Run:  .venv/bin/python inject_test.py [freq_hz] [amp_pct] [profile_hex]
"""
import sys, serial, socket, struct, time, threading, glob
import numpy as np
from scipy import signal as sps
import soundfile as sf

SR = 8000
FREQ = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
AMP = int(sys.argv[2]) if len(sys.argv) > 2 else 50
PROF = sys.argv[3] if len(sys.argv) > 3 else "0x30"


def port():
    p = glob.glob("/dev/cu.usbmodem*")
    return p[0] if p else "/dev/cu.usbmodem101"


def reset():
    s = serial.Serial(port(), 115200)
    s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False); s.close()


print(f"resetting device... (inject {FREQ}Hz @ {AMP}% through profile {PROF})")
try:
    reset()
except Exception as e:
    print("reset note:", e)
time.sleep(18)

s = serial.Serial(port(), 115200, timeout=1)
time.sleep(0.3); s.reset_input_buffer()


def cmd(c, w=0.6):
    s.write((c + "\n").encode()); s.flush(); time.sleep(w)
    return s.read(s.in_waiting).decode("utf-8", "replace")


def line(resp, key):
    hits = [l for l in resp.splitlines() if key in l]
    return hits[-1] if hits else "(none)"


st = cmd("T:CALL_STATE")
if "T:OK" not in st:
    print("device NOT responding after reset:", line(st, "T:")); sys.exit(1)
print("device up:", line(st, "T:OK"))

# UDP receiver
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", 9998))
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                struct.pack("=4sl", socket.inet_aton("239.0.99.99"), socket.INADDR_ANY))
sock.settimeout(0.5)
chunks = {}; maxend = [0]; run = [True]
def rx():
    while run[0]:
        try:
            d, _ = sock.recvfrom(2048)
            if len(d) >= 4:
                off = struct.unpack("<I", d[:4])[0]; chunks[off] = d[4:]
                maxend[0] = max(maxend[0], off + len(d) - 4)
        except Exception:
            pass
threading.Thread(target=rx, daemon=True).start()

cmd("T:BLE off", 1.8)
print("profile :", line(cmd(f"T:CALL_PROFILE {PROF}"), "T:OK"))
print("loopback:", line(cmd("T:LOOPBACK on", 1.0), "T:OK"))
if "T:OK" not in cmd("T:CALL_STATE"):
    print("device crashed after T:LOOPBACK on"); sys.exit(2)
print("inject  :", line(cmd(f"T:CALL_INJECT on {FREQ} {AMP}", 0.6), "T:"))
time.sleep(3)
cmd("T:CALL_INJECT off"); cmd("T:LOOPBACK off")
run[0] = False; time.sleep(0.3); s.close(); sock.close()

buf = bytearray(maxend[0])
for off, p in chunks.items():
    e = min(off + len(p), maxend[0]); buf[off:e] = p[:e - off]
pcm = np.frombuffer(bytes(buf), dtype="<i2").astype(float) / 32768.0
print(f"captured {len(pcm)/SR:.2f}s, {len(chunks)} chunks")
if len(pcm) > SR // 2:
    f, pw = sps.welch(pcm, SR, nperseg=1024); pk = float(f[np.argmax(pw)])
    conc = float(np.sum(pw[(f >= pk - 100) & (f <= pk + 100)]) / np.sum(pw))
    rms = float(np.sqrt(np.mean(pcm ** 2)))
    print(f"rms={rms:.4f}  peakFreq={pk:.0f}Hz  energy_concentration(+-100Hz)={conc:.2f}")
    clean = conc > 0.45 and abs(pk - FREQ) < 250
    print("VERDICT: CLEAN TONE -> codec OK, the MIC capture is the problem"
          if clean else "VERDICT: GARBAGE/broadband -> the CODEC/encode-decode path is broken")
    sf.write(f"out/inject_{FREQ}hz.wav", pcm, SR); print(f"saved out/inject_{FREQ}hz.wav")
else:
    print("no/short capture - loopback may have failed to stream")
