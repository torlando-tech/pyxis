#!/usr/bin/env python3
import glob, math, os, re, sys, threading, time
from types import SimpleNamespace

# Homebrew's arm64 libopus is not in dyld's default search path for the
# Xcode-provided Python used by the Mac Reticulum venv. Re-exec before LXST is
# imported so PyOgg can construct the default incoming-call codec.
if sys.platform == "darwin" and not os.environ.get("PYXIS_DYLD_READY"):
    env = os.environ.copy()
    paths = [p for p in env.get("DYLD_LIBRARY_PATH", "").split(":") if p]
    if "/opt/homebrew/lib" not in paths:
        paths.insert(0, "/opt/homebrew/lib")
    env["DYLD_LIBRARY_PATH"] = ":".join(paths)
    env["PYXIS_DYLD_READY"] = "1"
    os.execve(sys.executable, [sys.executable] + sys.argv, env)

HOME = os.path.expanduser("~")
sys.path.insert(0, os.path.join(HOME, "repos", "LXST"))
sys.path.insert(0, os.path.join(HOME, "repos", "Sideband", "sbapp"))
sys.path.append(os.path.join(HOME, "Library", "Python", "3.9", "lib", "python", "site-packages"))

import numpy as np
import serial
import RNS
import LXST.Sources as Sources
import LXST.Sinks as Sinks
from LXST.Codecs.Codec2 import Codec2
from LXST.Primitives.Telephony import Profiles
from sideband.voice import ReticulumTelephone

PORT = os.environ.get("PYXIS_SERIAL_PORT") or sorted(glob.glob("/dev/cu.usbmodem*"))[0]
PROFILE = Profiles.BANDWIDTH_ULTRA_LOW
RUN_SECONDS = float(os.environ.get("PYXIS_CALL_SECONDS", "7"))

class Stats:
    lock = threading.Lock()
    encodes = 0
    encode_bytes = 0
    decodes = 0
    decode_samples = 0
    decode_sumsq = 0.0
    sink_frames = 0
    sink_samples = 0

orig_encode = Codec2.encode
orig_decode = Codec2.decode

def counted_encode(self, frame):
    out = orig_encode(self, frame)
    with Stats.lock:
        Stats.encodes += 1
        Stats.encode_bytes += len(out)
    return out

def counted_decode(self, frame):
    out = orig_decode(self, frame)
    with Stats.lock:
        Stats.decodes += 1
        Stats.decode_samples += int(out.size)
        Stats.decode_sumsq += float(np.sum(np.square(out.astype(np.float64))))
    return out

Codec2.encode = counted_encode
Codec2.decode = counted_decode

class FakeRecorder:
    def __init__(self):
        self.phase = 0
    def __enter__(self): return self
    def __exit__(self, *args): return False
    def record(self, numframes):
        n = int(numframes)
        idx = np.arange(n, dtype=np.float64) + self.phase
        t = idx / 8000.0
        env = 0.55 + 0.45*np.sin(2*math.pi*120*t)
        x = env*(0.55*np.sin(2*math.pi*730*t) + 0.30*np.sin(2*math.pi*1095*t) + 0.15*np.sin(2*math.pi*2409*t))
        self.phase += n
        time.sleep(n/8000.0)
        return (0.35*x).astype(np.float32).reshape(-1,1)

class FakeSourceBackend:
    SAMPLERATE = 8000
    def __init__(self, preferred_device=None, samplerate=8000):
        self.samplerate = 8000
        self.channels = 1
        self.bitdepth = 32
        self.device = SimpleNamespace(channels=1)
    def get_recorder(self, samples_per_frame=None): return FakeRecorder()
    def release_recorder(self): pass
    def flush(self): pass
    def all_microphones(self): return []
    def default_microphone(self): return None

class FakePlayer:
    def __enter__(self): return self
    def __exit__(self, *args): return False
    def play(self, frame):
        with Stats.lock:
            Stats.sink_frames += 1
            Stats.sink_samples += int(frame.size)

class FakeSinkBackend:
    SAMPLERATE = 8000
    def __init__(self, preferred_device=None, samplerate=8000):
        self.samplerate = 8000
        self.device = SimpleNamespace(channels=1)
    def get_player(self, samples_per_frame=None, low_latency=None): return FakePlayer()
    def release_player(self): pass
    def flush(self): pass
    def all_speakers(self): return []
    def default_speaker(self): return None

Sources.Backend = FakeSourceBackend
Sinks.Backend = FakeSinkBackend

class Dev:
    def __init__(self):
        self.s = serial.Serial(PORT, 115200, timeout=0.12)
        self.crash_trace = False
        time.sleep(0.4)
        self.s.reset_input_buffer()
    def cmd(self, line, timeout=4):
        self.s.write((line+"\n").encode()); self.s.flush()
        deadline = time.time()+timeout; lines=[]
        while time.time()<deadline:
            raw=self.s.readline()
            if not raw: continue
            text=raw.decode("utf-8","replace").strip()
            if text: lines.append(text)
            if "Guru Meditation" in text:
                self.crash_trace = True
            if text and (self.crash_trace or "LXST" in text or "Link" in text or "PANIC" in text):
                print(f"PYXIS_LOG {text}", flush=True)
            if self.crash_trace and "ELF file SHA256" in text:
                self.crash_trace = False
            if text.startswith("T:OK") or text.startswith("T:ERR"):
                return text, lines
        return None, lines
    def state(self):
        r,_=self.cmd("T:CALL_STATE")
        return r.split("state=",1)[1] if r and "state=" in r else "?"
    def wait_state(self, wanted, timeout=30):
        deadline=time.time()+timeout; seen=[]
        while time.time()<deadline:
            st=self.state(); seen.append(st)
            if st==wanted: return True,seen
            time.sleep(0.35)
        return False,seen
    def close(self): self.s.close()

def val(resp,key):
    m=re.search(rf"\b{re.escape(key)}=([0-9]+)",resp or "")
    return int(m.group(1)) if m else -1

def wait_path_rns(dest, timeout=30):
    deadline=time.time()+timeout
    while time.time()<deadline:
        if RNS.Transport.has_path(dest): return True
        RNS.Transport.request_path(dest)
        time.sleep(0.7)
    return False

def wait_path_pyxis(dev,desthex,timeout=45):
    deadline=time.time()+timeout
    while time.time()<deadline:
        r,_=dev.cmd("T:HASPATH "+desthex)
        # Firmware includes diagnostic suffixes (`mem=... mem_count=...`).
        if r and r.startswith("T:OK 1"): return True
        time.sleep(0.8)
    return False

class Owner:
    def __init__(self):
        self.config={"voice_trusted_only":False}
        self.events=[]
    def voice_is_trusted(self,h): return True
    def setstate(self,*a): self.events.append(("state",a))
    def incoming_call(self,i): self.events.append(("incoming",i.hash.hex()))
    def ended_call(self,i): self.events.append(("ended",i.hash.hex()))
    def missed_call(self,i): self.events.append(("missed",i.hash.hex()))

def snapshot():
    with Stats.lock:
        return dict(encodes=Stats.encodes, encode_bytes=Stats.encode_bytes, decodes=Stats.decodes,
                    decode_samples=Stats.decode_samples, decode_sumsq=Stats.decode_sumsq,
                    sink_frames=Stats.sink_frames, sink_samples=Stats.sink_samples)

def delta(a,b): return {k:b[k]-a[k] for k in a}

def run_audio(dev, label):
    before=snapshot()
    dev.cmd("T:CALL_INJECT on 730 50")
    time.sleep(RUN_SECONDS)
    dev.cmd("T:CALL_INJECT off")
    qos,_=dev.cmd("T:CALL_QOS")
    stat,_=dev.cmd("T:CALL_STATS")
    after=snapshot(); d=delta(before,after)
    rms=(d["decode_sumsq"]/d["decode_samples"])**0.5 if d["decode_samples"]>0 else 0
    print(f"{label} SIDEBAND_DELTA={d} sideband_decode_rms={rms:.5f}",flush=True)
    print(f"{label} PYXIS_QOS={qos}",flush=True)
    print(f"{label} PYXIS_STATS={stat}",flush=True)
    ok=(d["encodes"]>0 and d["decodes"]>0 and d["decode_samples"]>0 and
        val(qos,"decode_ok")>0 and val(qos,"decode_fail")==0 and val(qos,"pcm_n")>0)
    return ok,{"sideband":d,"qos":qos,"stats":stat,"rms":rms}

def main():
    print(f"PORT={PORT}",flush=True)
    dev=Dev()
    owner=Owner()
    # Use a fresh identity and isolated RNS storage on every run so cached paths
    # cannot make wait_path_rns() pass before the current Pyxis announce arrives.
    identity=RNS.Identity()
    config_dir=f"/tmp/pyxis-sideband-rns-{os.getpid()}"
    os.makedirs(config_dir,exist_ok=True)
    rns_host=os.environ.get("PYXIS_RNS_HOST","127.0.0.1")
    rns_port=int(os.environ.get("PYXIS_RNS_PORT","4242"))
    with open(os.path.join(config_dir,"config"),"w") as f:
        f.write(f"""[reticulum]\nenable_transport = No\nshare_instance = No\nshared_instance_port = 48428\ninstance_control_port = 48429\n\n[logging]\nloglevel = 5\n\n[interfaces]\n  [[Pyxis troubleshooting hub]]\n    type = TCPClientInterface\n    enabled = yes\n    target_host = {rns_host}\n    target_port = {rns_port}\n""")
    print(f"RNS_TEST_HUB={rns_host}:{rns_port}",flush=True)
    reticulum=RNS.Reticulum(configdir=config_dir,loglevel=5)
    phone=ReticulumTelephone(identity, owner=owner)
    phone.telephone.auto_answer=0.6
    phone.announce()
    side_id=identity.hash.hex(); side_dest=phone.telephone.destination.hash.hex()
    py_id=(dev.cmd("T:ID")[0] or "").split()[-1]
    py_dest=(dev.cmd("T:LXSTDEST")[0] or "").split()[-1]
    print(f"SIDE_ID={side_id} SIDE_DEST={side_dest}",flush=True)
    print(f"PYXIS_ID={py_id} PYXIS_DEST={py_dest}",flush=True)
    dev.cmd("T:BLE off",timeout=5)
    dev.cmd("T:CALL_PROFILE 0x10")
    dev.cmd("T:ANNLXST")
    phone.announce()
    results=[]
    try:
        # Pyxis -> Sideband first. This proves Pyxis learned the fresh peer
        # announce before testing the reciprocal incoming route.
        phone.announce(); dev.cmd("T:ANNLXST")
        assert wait_path_pyxis(dev,side_dest,45),"Pyxis did not learn Sideband LXST path"
        print("TEST1 dialing Pyxis -> Sideband",flush=True)
        print("TEST1 call",dev.cmd("T:CALL "+side_dest)[0],flush=True)
        ok,seen=dev.wait_state("ACTIVE",35); print("TEST1 states_to_active",seen,flush=True); assert ok
        deadline=time.time()+15
        while not phone.is_in_call and time.time()<deadline: time.sleep(.2)
        assert phone.is_in_call,"Sideband did not become active on incoming call"
        ok,data=run_audio(dev,"TEST1")
        results.append(("pyxis_to_sideband_bidirectional",ok,data))
        dev.cmd("T:CALL_HANGUP"); dev.wait_state("IDLE",15); time.sleep(1)

        # Sideband -> Pyxis after a completed outgoing call.
        dev.cmd("T:ANNLXST"); phone.announce()
        assert wait_path_rns(bytes.fromhex(py_dest),45),"Sideband did not learn Pyxis LXST path"
        print("TEST2 dialing Sideband -> Pyxis",flush=True)
        assert phone.dial(bytes.fromhex(py_id),profile=PROFILE)!="no_path"
        ok,seen=dev.wait_state("INCOMING_RINGING",25); print("TEST2 states_to_ring",seen,flush=True); assert ok
        print("TEST2 answer",dev.cmd("T:CALL_ANSWER")[0],flush=True)
        ok,seen=dev.wait_state("ACTIVE",25); print("TEST2 states_to_active",seen,flush=True); assert ok
        deadline=time.time()+15
        while not phone.is_in_call and time.time()<deadline: time.sleep(.2)
        assert phone.is_in_call,"Sideband did not become active"
        ok,data=run_audio(dev,"TEST2")
        results.append(("sideband_to_pyxis_bidirectional",ok,data))
        phone.hangup(); dev.wait_state("IDLE",15); time.sleep(1)

        # Regression: incoming callback must still work after prior hangups.
        dev.cmd("T:ANNLXST"); assert wait_path_rns(bytes.fromhex(py_dest),20)
        print("TEST3 redial Sideband -> Pyxis after completed calls",flush=True)
        r=phone.dial(bytes.fromhex(py_id),profile=PROFILE)
        ok,seen=dev.wait_state("INCOMING_RINGING",15); print("TEST3 states_to_ring",seen,flush=True)
        results.append(("incoming_after_hangups",ok,{"dial":r,"states":seen}))
        if ok: dev.cmd("T:CALL_HANGUP")
        elif phone.telephone.active_call: phone.hangup()
    finally:
        try: dev.cmd("T:CALL_INJECT off")
        except Exception: pass
        try:
            if phone.telephone and phone.telephone.active_call: phone.hangup()
            phone.stop()
        except Exception as e: print("PHONE_CLEANUP",repr(e),flush=True)
        try: dev.cmd("T:BLE on",timeout=5)
        except Exception: pass
        dev.close()
    print("RESULTS",results,flush=True)
    return 0 if all(x[1] for x in results) else 1

if __name__=="__main__":
    try: raise SystemExit(main())
    except Exception as e:
        import traceback; traceback.print_exc(); raise SystemExit(2)
