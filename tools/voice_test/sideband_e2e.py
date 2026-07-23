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
from RNS.vendor import umsgpack as msgpack
import LXST.Sources as Sources
import LXST.Sinks as Sinks
from LXST.Codecs.Codec2 import Codec2
from LXST.Primitives.Telephony import Profiles
from sideband.voice import ReticulumTelephone

PORT = os.environ.get("PYXIS_SERIAL_PORT") or sorted(glob.glob("/dev/cu.usbmodem*"))[0]
PROFILE = Profiles.BANDWIDTH_ULTRA_LOW
RUN_SECONDS = float(os.environ.get("PYXIS_CALL_SECONDS", "7"))
IDENTIFY_TIMEOUT_SECONDS = 15.0
IDENTIFY_TIMEOUT_MARGIN = float(os.environ.get("PYXIS_IDENTIFY_TIMEOUT_MARGIN", "3"))

if not math.isfinite(IDENTIFY_TIMEOUT_MARGIN) or IDENTIFY_TIMEOUT_MARGIN <= 0:
    raise ValueError("PYXIS_IDENTIFY_TIMEOUT_MARGIN must be a positive finite number")

FIELD_SIGNALLING = 0x00
STATUS_BUSY = 0x00
STATUS_AVAILABLE = 0x03
STATUS_RINGING = 0x04

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
        try:
            self.crash_trace = False
            time.sleep(0.4)
            self.s.reset_input_buffer()
        except BaseException:
            self.s.close()
            raise
    def cmd(self, line, timeout=4):
        self.s.write((line+"\n").encode()); self.s.flush()
        deadline = time.monotonic()+timeout; lines=[]
        while time.monotonic()<deadline:
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
        deadline=time.monotonic()+timeout; seen=[]
        while time.monotonic()<deadline:
            st=self.state(); seen.append(st)
            if st==wanted: return True,seen
            time.sleep(0.35)
        return False,seen
    def close(self): self.s.close()

class RawCaller:
    """A deliberately manual LXST caller backed by one fresh RNS identity.

    This uses the ordinary RNS Link API directly instead of Telephone, since
    Telephone automatically identifies as soon as STATUS_AVAILABLE arrives.
    """
    def __init__(self, destination_hash, label):
        self.label = label
        self.identity = RNS.Identity()
        self.destination_hash = bytes(destination_hash)
        remote_identity = RNS.Identity.recall(self.destination_hash)
        if remote_identity is None:
            raise RuntimeError(
                f"{label}: no recalled identity for LXST destination "
                f"{self.destination_hash.hex()}; wait for a current Pyxis announce"
            )
        self.destination = RNS.Destination(
            remote_identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
            "lxst", "telephony")
        if self.destination.hash != self.destination_hash:
            raise RuntimeError(
                f"{label}: recalled identity produced {self.destination.hash.hex()}, "
                f"expected {self.destination_hash.hex()}"
            )
        self._established = threading.Event()
        self._closed = threading.Event()
        self._condition = threading.Condition()
        self._signals = []
        self._packet_errors = []
        self.link = RNS.Link(
            self.destination,
            established_callback=self._on_established,
            closed_callback=self._on_closed)
        # Ordinary Link packets are dropped when no packet callback is present.
        # Register at the earliest point supported by the public RNS API. A very
        # small constructor-return window remains because Link.__init__() does
        # not accept a packet callback.
        self.link.set_packet_callback(self._on_packet)

    def _on_established(self, link):
        # Harmlessly repeat registration in case the Link implementation resets
        # callbacks while transitioning to ACTIVE.
        link.set_packet_callback(self._on_packet)
        with self._condition:
            self._established.set()
            self._condition.notify_all()
        print(f"{self.label} RAW_LINK_ESTABLISHED id={link.link_id.hex()}", flush=True)

    def _on_closed(self, link):
        self._closed.set()
        with self._condition:
            self._condition.notify_all()
        print(f"{self.label} RAW_LINK_CLOSED reason={link.teardown_reason}", flush=True)

    def _on_packet(self, data, packet):
        try:
            unpacked = msgpack.unpackb(data)
            if not isinstance(unpacked, dict) or FIELD_SIGNALLING not in unpacked:
                return
            signals = unpacked[FIELD_SIGNALLING]
            if not isinstance(signals, list):
                signals = [signals]
            if not all(isinstance(signal, int) for signal in signals):
                raise ValueError(f"non-integer signalling values: {signals!r}")
            with self._condition:
                self._signals.extend(signals)
                self._condition.notify_all()
            print(f"{self.label} RAW_SIGNALS={signals}", flush=True)
        except Exception as exc:
            with self._condition:
                self._packet_errors.append(repr(exc))
                self._condition.notify_all()
            print(f"{self.label} RAW_PACKET_ERROR={exc!r}", flush=True)

    @property
    def signals(self):
        with self._condition:
            return tuple(self._signals)

    @property
    def is_open(self):
        return self.link.status != RNS.Link.CLOSED

    def wait_established(self, timeout=20):
        deadline = time.monotonic()+timeout
        with self._condition:
            while not self._established.is_set():
                if self._packet_errors:
                    raise AssertionError(
                        f"{self.label}: failed to decode LXST packet(s) before establishment: "
                        f"{self._packet_errors}")
                if self._closed.is_set() or self.link.status == RNS.Link.CLOSED:
                    raise AssertionError(
                        f"{self.label}: raw link closed before establishment "
                        f"(status={self.link.status}, signals={self._signals})")
                remaining = deadline-time.monotonic()
                if remaining <= 0:
                    raise AssertionError(
                        f"{self.label}: raw link did not establish in {timeout}s "
                        f"(status={self.link.status}, signals={self._signals}, "
                        f"closed={self._closed.is_set()})")
                # Poll status too: some proof-validation failures set CLOSED
                # without invoking the close callback.
                self._condition.wait(min(0.1, max(0, remaining)))
        return self

    def wait_signal(self, signal, timeout=8):
        deadline = time.monotonic()+timeout
        close_drain_deadline = None
        with self._condition:
            while signal not in self._signals:
                now = time.monotonic()
                if self._closed.is_set() or self.link.status == RNS.Link.CLOSED:
                    # Packet callbacks run on worker threads while close can be
                    # synchronous. Allow an already-queued BUSY packet to drain.
                    if close_drain_deadline is None:
                        close_drain_deadline = min(deadline, now+1.0)
                effective_deadline = (min(deadline, close_drain_deadline)
                                      if close_drain_deadline is not None else deadline)
                remaining = effective_deadline-now
                if remaining <= 0:
                    break
                self._condition.wait(max(0, remaining))
            if self._packet_errors:
                raise AssertionError(
                    f"{self.label}: failed to decode LXST packet(s): {self._packet_errors}")
            if signal not in self._signals:
                closed = self._closed.is_set() or self.link.status == RNS.Link.CLOSED
                raise AssertionError(
                    f"{self.label}: did not receive signal 0x{signal:02x} in {timeout}s; "
                    f"received={self._signals}, closed={closed}")
        return self

    def wait_closed(self, timeout=8):
        if not self._closed.wait(timeout):
            raise AssertionError(
                f"{self.label}: link did not close in {timeout}s "
                f"(status={self.link.status}, signals={self.signals})")
        return self

    def identify(self, allow_closed=False):
        if not self._established.is_set():
            raise RuntimeError(f"{self.label}: cannot identify before link establishment")
        if not self.is_open:
            if allow_closed:
                # RNS identify() intentionally becomes a no-op unless the
                # initiator link is ACTIVE. Calling it exercises the feasible
                # late-action path without fabricating a Reticulum callback.
                self.link.identify(self.identity)
                return False
            raise RuntimeError(f"{self.label}: cannot identify on a closed link")
        self.link.identify(self.identity)
        return True

    def close(self, timeout=8):
        if self.link.status != RNS.Link.CLOSED:
            self.link.teardown()
        if self.link.status != RNS.Link.CLOSED or self._closed.is_set():
            self.wait_closed(timeout)

def assert_state_for(dev, wanted, duration, label):
    deadline=time.monotonic()+duration; seen=[]
    while time.monotonic()<deadline:
        state=dev.state(); seen.append(state)
        if state != wanted:
            raise AssertionError(
                f"{label}: expected state {wanted} for {duration}s, "
                f"observed {state}; history={seen}")
        time.sleep(0.25)
    return seen

def cleanup_raw_callers(dev, *callers):
    primary = sys.exc_info()[1]
    errors = []
    try:
        if dev.state() != "IDLE": dev.cmd("T:CALL_HANGUP")
    except BaseException as exc:
        errors.append(f"device hangup: {exc!r}")
    for caller in callers:
        if caller is not None:
            try: caller.close()
            except BaseException as exc:
                errors.append(f"{caller.label}: {exc!r}")
    try:
        ok,seen=dev.wait_state("IDLE",15)
        if not ok:
            raise AssertionError(f"raw-call cleanup did not reach IDLE; states={seen}")
    except BaseException as exc:
        errors.append(f"device idle: {exc!r}")
    if errors:
        detail = "; ".join(errors)
        if primary is not None:
            print(f"RAW_CLEANUP_AFTER_PRIMARY primary={primary!r} cleanup={detail}", flush=True)
        else:
            raise AssertionError(f"raw-call cleanup failed: {detail}")

def val(resp,key):
    m=re.search(rf"\b{re.escape(key)}=([0-9]+)",resp or "")
    return int(m.group(1)) if m else -1

def wait_path_rns(dest, timeout=30):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if RNS.Transport.has_path(dest): return True
        RNS.Transport.request_path(dest)
        time.sleep(0.7)
    return False

def wait_path_pyxis(dev,desthex,timeout=45):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
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
    print(f"RNS_VERSION={RNS.__version__}",flush=True)
    dev=None
    phone=None
    initial_ble_enabled=None
    results=[]
    try:
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
        ble_response,_=dev.cmd("T:BLE",timeout=5)
        ble_match=re.search(r"\bble_enabled=([01])\b",ble_response or "")
        assert ble_match, f"could not query initial BLE state: {ble_response!r}"
        initial_ble_enabled = ble_match.group(1) == "1"
        if initial_ble_enabled:
            response,_=dev.cmd("T:BLE off",timeout=5)
            assert response and response.startswith("T:OK"), \
                f"could not disable BLE for voice harness: {response!r}"
        dev.cmd("T:CALL_PROFILE 0x10")
        dev.cmd("T:ANNLXST")
        phone.announce()

        # Both directions need a current announce before the raw-link cases.
        phone.announce(); dev.cmd("T:ANNLXST")
        assert wait_path_pyxis(dev,side_dest,45),"Pyxis did not learn Sideband LXST path"
        assert wait_path_rns(bytes.fromhex(py_dest),45),"Sideband did not learn Pyxis LXST path"

        # Contention 1: B must be rejected without displacing unidentified A.
        a=b=None
        try:
            print("TEST_IDENTIFY_CONTENTION raw A then raw B",flush=True)
            a=RawCaller(bytes.fromhex(py_dest),"CONTENTION_A")
            a.wait_established()
            a.wait_signal(STATUS_AVAILABLE)
            ok,seen=dev.wait_state("INCOMING_IDENTIFYING",10)
            print("TEST_IDENTIFY_CONTENTION states_to_identifying",seen,flush=True)
            assert ok,"Pyxis did not reserve caller A while awaiting identity"
            b=RawCaller(bytes.fromhex(py_dest),"CONTENTION_B")
            b.wait_established()
            b.wait_signal(STATUS_BUSY).wait_closed()
            assert_state_for(dev,"INCOMING_IDENTIFYING",1.0,"caller B rejection")
            assert a.is_open,"caller A was closed when caller B was rejected"
            a.identify(); a.wait_signal(STATUS_RINGING)
            ok,seen=dev.wait_state("INCOMING_RINGING",10)
            assert ok,f"caller A did not ring after identification; states={seen}"
            results.append(("unidentified_owner_rejects_second_link",True,
                            {"a_signals":a.signals,"b_signals":b.signals,"states":seen}))
        finally:
            cleanup_raw_callers(dev,a,b)

        # Contention 2: a local outgoing request cannot steal A's reservation.
        a=None
        try:
            print("TEST_IDENTIFY_OUTGOING outgoing request while raw A owns",flush=True)
            a=RawCaller(bytes.fromhex(py_dest),"OUTGOING_RACE_A")
            a.wait_established()
            a.wait_signal(STATUS_AVAILABLE)
            ok,seen=dev.wait_state("INCOMING_IDENTIFYING",10)
            assert ok,f"Pyxis did not enter identifying before outgoing race; states={seen}"
            response,_=dev.cmd("T:CALL "+side_dest)
            assert response == "T:ERR busy", \
                f"T:CALL did not prove exact busy admission rejection: {response!r}"
            stable=assert_state_for(dev,"INCOMING_IDENTIFYING",1.0,"outgoing initiation")
            assert a.is_open,"outgoing initiation displaced caller A"
            assert not phone.is_in_call and not phone.telephone.active_call, \
                "Sideband received an outgoing call while caller A owned Pyxis"
            a.identify(); a.wait_signal(STATUS_RINGING)
            ok,seen=dev.wait_state("INCOMING_RINGING",10)
            assert ok,f"caller A did not ring after outgoing race; states={seen}"
            results.append(("outgoing_cannot_displace_unidentified_owner",True,
                            {"call_response":response,"stable":stable,"a_signals":a.signals}))
        finally:
            cleanup_raw_callers(dev,a)

        # A closed link's late identify API is a no-op. Verify close + B redial
        # stability while any already-queued callbacks from A drain.
        a=b=None
        try:
            print("TEST_CLOSED_LINK_NOOP close A then identify B",flush=True)
            a=RawCaller(bytes.fromhex(py_dest),"CLOSED_A")
            a.wait_established()
            a.wait_signal(STATUS_AVAILABLE)
            ok,seen=dev.wait_state("INCOMING_IDENTIFYING",10)
            assert ok,f"Pyxis did not reserve caller A before close; states={seen}"
            a.close(); ok,seen=dev.wait_state("IDLE",15)
            assert ok,f"Pyxis did not release caller A; states={seen}"
            b=RawCaller(bytes.fromhex(py_dest),"REDIAL_B")
            b.wait_established()
            b.wait_signal(STATUS_AVAILABLE); b.identify(); b.wait_signal(STATUS_RINGING)
            ok,seen=dev.wait_state("INCOMING_RINGING",10)
            assert ok,f"caller B did not ring; states={seen}"
            late_sent=a.identify(allow_closed=True)
            assert not late_sent,"closed caller A unexpectedly sent a late identification"
            stable=assert_state_for(dev,"INCOMING_RINGING",1.0,"closed caller A callback drain")
            assert b.is_open,"closed caller A drain disturbed current caller B"
            results.append(("closed_link_identify_noop_and_redial_stability",True,
                            {"late_identify_sent":late_sent,"stable":stable,"b_signals":b.signals}))
        finally:
            cleanup_raw_callers(dev,a,b)

        # Identification timeout must close A and leave the incoming destination
        # reusable for a complete, audio-bearing Sideband call.
        a=None
        try:
            print("TEST_IDENTIFY_TIMEOUT wait for raw A timeout",flush=True)
            a=RawCaller(bytes.fromhex(py_dest),"TIMEOUT_A")
            a.wait_established()
            a.wait_signal(STATUS_AVAILABLE)
            ok,seen=dev.wait_state("INCOMING_IDENTIFYING",10)
            assert ok,f"Pyxis did not enter identifying before timeout; states={seen}"
            wait_seconds=IDENTIFY_TIMEOUT_SECONDS+IDENTIFY_TIMEOUT_MARGIN
            print(f"TEST_IDENTIFY_TIMEOUT sleeping={wait_seconds:.1f}s",flush=True)
            time.sleep(wait_seconds)
            ok,seen=dev.wait_state("IDLE",5)
            assert ok, \
                f"Pyxis was not IDLE after {wait_seconds:.1f}s identification timeout; states={seen}"
            a.wait_closed(5)
        finally:
            cleanup_raw_callers(dev,a)

        dev.cmd("T:ANNLXST"); assert wait_path_rns(bytes.fromhex(py_dest),20)
        print("TEST_IDENTIFY_TIMEOUT recovery Sideband -> Pyxis",flush=True)
        recovery_dial=phone.dial(bytes.fromhex(py_id),profile=PROFILE)
        assert recovery_dial!="no_path","timeout recovery call had no path"
        ok,seen=dev.wait_state("INCOMING_RINGING",25)
        assert ok,f"timeout recovery call did not ring; states={seen}"
        print("TEST_IDENTIFY_TIMEOUT answer",dev.cmd("T:CALL_ANSWER")[0],flush=True)
        ok,seen_active=dev.wait_state("ACTIVE",25)
        assert ok,f"timeout recovery call did not become active; states={seen_active}"
        deadline=time.monotonic()+15
        while not phone.is_in_call and time.monotonic()<deadline: time.sleep(.2)
        assert phone.is_in_call,"Sideband did not become active after identification timeout"
        ok,data=run_audio(dev,"TEST_IDENTIFY_TIMEOUT")
        phone.hangup()
        idle_ok,hangup_states=dev.wait_state("IDLE",15)
        assert idle_ok, \
            f"timeout recovery hangup did not return Pyxis to IDLE; states={hangup_states}"
        hangup_deadline=time.monotonic()+15
        while (phone.is_in_call or phone.telephone.active_call) and time.monotonic()<hangup_deadline:
            time.sleep(.2)
        assert not phone.is_in_call and not phone.telephone.active_call, \
            "timeout recovery hangup did not close the Sideband call/link"
        results.append(("identification_timeout_recovery_bidirectional",ok,
                        {"dial":recovery_dial,"ring_states":seen,"audio":data,
                         "hangup_states":hangup_states}))
        time.sleep(1)

        # Pyxis -> Sideband first. This proves Pyxis learned the fresh peer
        # announce before testing the reciprocal incoming route.
        phone.announce(); dev.cmd("T:ANNLXST")
        assert wait_path_pyxis(dev,side_dest,45),"Pyxis did not learn Sideband LXST path"
        print("TEST1 dialing Pyxis -> Sideband",flush=True)
        print("TEST1 call",dev.cmd("T:CALL "+side_dest)[0],flush=True)
        ok,seen=dev.wait_state("ACTIVE",35); print("TEST1 states_to_active",seen,flush=True); assert ok
        deadline=time.monotonic()+15
        while not phone.is_in_call and time.monotonic()<deadline: time.sleep(.2)
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
        deadline=time.monotonic()+15
        while not phone.is_in_call and time.monotonic()<deadline: time.sleep(.2)
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
        primary = sys.exc_info()[1]
        cleanup_errors = []
        if dev is not None:
            try: dev.cmd("T:CALL_INJECT off")
            except BaseException as exc: cleanup_errors.append(f"inject off: {exc!r}")
        if phone is not None:
            try:
                if phone.telephone and phone.telephone.active_call: phone.hangup()
            except BaseException as exc:
                cleanup_errors.append(f"phone hangup: {exc!r}")
            try: phone.stop()
            except BaseException as exc: cleanup_errors.append(f"phone stop: {exc!r}")
        if dev is not None and initial_ble_enabled is not None:
            try:
                restore = "on" if initial_ble_enabled else "off"
                response,_=dev.cmd("T:BLE "+restore,timeout=5)
                if not response or not response.startswith("T:OK"):
                    raise AssertionError(f"unexpected response {response!r}")
            except BaseException as exc: cleanup_errors.append(f"BLE restore: {exc!r}")
        if dev is not None:
            try: dev.close()
            except BaseException as exc: cleanup_errors.append(f"serial close: {exc!r}")
        if cleanup_errors:
            detail = "; ".join(cleanup_errors)
            if primary is not None:
                print(f"CLEANUP_AFTER_PRIMARY primary={primary!r} cleanup={detail}", flush=True)
            else:
                raise AssertionError(f"harness cleanup failed: {detail}")
    print("RESULTS",results,flush=True)
    return 0 if all(x[1] for x in results) else 1

if __name__=="__main__":
    try: raise SystemExit(main())
    except Exception as e:
        import traceback; traceback.print_exc(); raise SystemExit(2)
