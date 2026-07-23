#!/usr/bin/env python3
"""Inject a sweep of known tones through the loopback and measure the output peak
frequency for each. If output/input ratio is a constant != 1.0 -> sample-rate
mismatch. If ~1.0 with high concentration -> codec faithfully preserves frequency
(mic is the problem). If random/low concentration -> codec garbage."""
import serial, socket, struct, time, threading, glob
import numpy as np
from scipy import signal as sps

SR = 8000
FREQS = [300, 500, 800, 1200, 1800, 2400]


def port():
    p = glob.glob("/dev/cu.usbmodem*"); return p[0] if p else "/dev/cu.usbmodem101"


s0 = serial.Serial(port(), 115200); s0.setDTR(False); s0.setRTS(True); time.sleep(0.2); s0.setRTS(False); s0.close()
print("reset; waiting for boot..."); time.sleep(18)

s = serial.Serial(port(), 115200, timeout=1); time.sleep(0.3); s.reset_input_buffer()
def cmd(c, w=0.6):
    s.write((c + "\n").encode()); s.flush(); time.sleep(w)
    return s.read(s.in_waiting).decode("utf-8", "replace")
def lastok(r):
    h = [l for l in r.splitlines() if "T:OK" in l]; return h[-1] if h else "(none)"

if "T:OK" not in cmd("T:CALL_STATE"):
    print("device not responding"); raise SystemExit(1)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); sock.bind(("", 9998))
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                struct.pack("=4sl", socket.inet_aton("239.0.99.99"), socket.INADDR_ANY))
sock.settimeout(0.5)
chunks = {}; maxend = [0]; run = [True]; lock = threading.Lock()
def rx():
    while run[0]:
        try:
            d, _ = sock.recvfrom(2048)
            if len(d) >= 4:
                off = struct.unpack("<I", d[:4])[0]
                with lock:
                    chunks[off] = d[4:]; maxend[0] = max(maxend[0], off + len(d) - 4)
        except Exception: pass
threading.Thread(target=rx, daemon=True).start()

cmd("T:BLE off", 1.8)
print("profile :", lastok(cmd("T:CALL_PROFILE 0x30")))
print("loopback:", lastok(cmd("T:LOOPBACK on", 1.0)))
if "T:OK" not in cmd("T:CALL_STATE"):
    print("device crashed after loopback on"); raise SystemExit(2)

print(f"\n{'inject':>7} {'outPeak':>8} {'ratio':>6} {'conc':>5}")
for fq in FREQS:
    cmd(f"T:CALL_INJECT on {fq} 50", 0.3)
    time.sleep(1.3)                          # let old audio drain (loopback latency)
    with lock:
        chunks.clear(); maxend[0] = 0
    time.sleep(1.6)                          # capture the new tone
    with lock:
        end = maxend[0]; snap = dict(chunks)
    buf = bytearray(end)
    for off, p in snap.items():
        e = min(off + len(p), end); buf[off:e] = p[:e - off]
    pcm = np.frombuffer(bytes(buf), dtype="<i2").astype(float) / 32768.0
    if len(pcm) < SR // 2:
        print(f"{fq:7} {'short':>8}"); continue
    f, pw = sps.welch(pcm, SR, nperseg=1024); pk = float(f[np.argmax(pw)])
    conc = float(np.sum(pw[(f >= pk - 100) & (f <= pk + 100)]) / np.sum(pw))
    print(f"{fq:7} {pk:8.0f} {pk/fq:6.3f} {conc:5.2f}")

cmd("T:CALL_INJECT off"); cmd("T:LOOPBACK off")
run[0] = False; s.close(); sock.close()
print("\nIf ratio is a consistent value != 1.0 -> SAMPLE-RATE MISMATCH.")
print("If ratio ~1.0 + conc high -> codec preserves freq (mic is the problem).")
