#!/usr/bin/env python3
"""
T-Deck ↔ Mac echo-bot integration harness.

This script drives a pyxis-built T-Deck firmware (with -DPYXIS_TEST_HOOKS)
and a Mac-side LXMF echo bot together, exercising LXMF DIRECT delivery
in both directions over the local rnsd's TCPServerInterface
(host:port configured at firmware-build-time via PYXIS_TEST_TCP_HOST /
PYXIS_TEST_TCP_PORT in platformio.ini).

What it does:

1. Open the T-Deck serial port (/dev/cu.usbmodem1101) and reset the device.
   Stream every line into a captured log; parse `T:OK` / `T:ERR` /
   `T:RXMSG` / `T:PATH` lines for the harness, pass everything else
   through to the log.

2. Wait for pyxis boot: `BOOT END: ui_manager` line, then a TCP
   connect-success log to confirm the rnsd link is up.

3. Start the echo bot AFTER pyxis is fully up (subprocess) so its
   announce arrives while pyxis is listening. Bot uses the same rnsd
   the T-Deck talks to (shared instance via shared_instance_port).

4. Wait for pyxis to learn the bot's path (`T:HASPATH <bot_dest>`
   returns `T:OK 1`), with bot manually re-announcing if needed.

5. Test loop, repeating until either a failure or the soak deadline:
   - Send short message from pyxis to bot via `T:SEND`.
   - Wait for bot to log RX + ECHO SENT.
   - Wait for pyxis to log RX (poll `T:RX`).
   - Verify content matches `echo: <orig>`.
   - Send long (~1KB) message and repeat.
   - Sleep `--cadence` seconds between rounds.

6. Soak: run for at least `--soak-hours` hours (default 1.1). Crash =
   fail. Mid-test failures are logged but don't abort.

Usage:
    # Run with a python that has pyserial available. PlatformIO's
    # bundled python works:
    "$(pio system info | awk -F: '/Python Executable/{print $2}' | xargs)" \\
        tests/soak/tdeck_soak_harness.py [--soak-hours 1.1] [--cadence 30]

Outputs:
    /tmp/tdeck-harness.log         — combined timestamped event log
    /tmp/tdeck-harness-tdeck.log   — verbatim serial output from T-Deck
    /tmp/echobot.log               — echo bot's own log (subprocess stdout)

Exits 0 on full success, 1 on failure.
"""
import os, sys, time, threading, subprocess, argparse, queue, signal
import datetime

# pyserial: try the active python first, fall back to PlatformIO's
# bundled site-packages if the user runs us through system python.
try:
    import serial  # noqa: F401
except ImportError:
    pio_site = os.environ.get("PIO_SITE_PACKAGES")
    if pio_site and os.path.isdir(pio_site):
        sys.path.insert(0, pio_site)
    import serial  # second try; let it raise if still missing

# Default serial port for a USB-attached T-Deck Plus on macOS. Override
# with PYXIS_SERIAL_PORT (eg "/dev/ttyUSB0" on Linux).
PORT = os.environ.get("PYXIS_SERIAL_PORT", "/dev/cu.usbmodem1101")
BAUD = 115200
HARNESS_LOG = "/tmp/tdeck-harness.log"
TDECK_LOG = "/tmp/tdeck-harness-tdeck.log"
ECHOBOT_LOG = "/tmp/echobot.log"
ECHOBOT_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "lxmf_echo_bot.py")
# Interpreter for the echo bot subprocess. Defaults to the same Python running
# this harness (which run_e2e.sh selects, and which already has rns/lxmf
# importable via the bot's repo-path insert). Override with PYXIS_BOT_PY if the
# bot needs a different interpreter than the harness.
BOT_PY = os.environ.get("PYXIS_BOT_PY") or sys.executable


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


_log_lock = threading.Lock()
_log_fh = None


def log(category, msg):
    line = f"[{ts()}] [{category}] {msg}\n"
    with _log_lock:
        sys.stdout.write(line)
        sys.stdout.flush()
        if _log_fh is not None:
            _log_fh.write(line)
            _log_fh.flush()


class TDeck:
    """Serial driver: reset, command, parse response."""

    def __init__(self, port=PORT, baud=BAUD):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self._line_q = queue.Queue()           # raw text lines
        self._tdeck_log = open(TDECK_LOG, "wb")
        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def reset(self):
        """Pulse DTR/RTS like esptool to reboot the ESP32 cleanly."""
        log("HARNESS", "Resetting T-Deck via DTR/RTS pulse")
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.dtr = True
        self.ser.rts = False
        time.sleep(0.1)
        self.ser.dtr = False
        self.ser.rts = False

    def _read_loop(self):
        buf = b""
        while not self._stop.is_set():
            try:
                d = self.ser.read(4096)
            except Exception as e:
                log("HARNESS", f"Serial read error: {e}")
                return
            if not d:
                continue
            self._tdeck_log.write(d)
            self._tdeck_log.flush()
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    text = line.rstrip(b"\r").decode("utf-8", errors="replace")
                except Exception:
                    text = repr(line)
                self._line_q.put(text)

    def drain_lines(self):
        """Pop all currently-buffered lines (non-blocking)."""
        out = []
        while True:
            try:
                out.append(self._line_q.get_nowait())
            except queue.Empty:
                break
        return out

    def wait_for_line(self, predicate, timeout):
        """Block until a line matches predicate(text) or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self._line_q.get(timeout=min(1.0, deadline - time.time()))
            except queue.Empty:
                continue
            if predicate(line):
                return line
        return None

    def send_command(self, cmd, response_timeout=10.0):
        """Send a `T:` command, wait for the next `T:OK` or `T:ERR` line.

        Returns the response line (without the `T:OK` / `T:ERR` prefix
        portion left intact for inspection), or None on timeout.
        """
        log("HARNESS-TX", cmd)
        # Drain any stale T: responses queued up from earlier
        deadline = time.time() + response_timeout
        # Send
        self.ser.write((cmd + "\n").encode("utf-8"))
        self.ser.flush()
        # Read until we see a T:OK or T:ERR
        while time.time() < deadline:
            try:
                line = self._line_q.get(timeout=min(1.0, deadline - time.time()))
            except queue.Empty:
                continue
            if line.startswith("T:OK") or line.startswith("T:ERR"):
                log("HARNESS-RX", line)
                return line
        log("HARNESS", f"send_command({cmd!r}) timed out")
        return None

    def close(self):
        self._stop.set()
        self.ser.close()
        self._tdeck_log.close()


def wait_for_pyxis_boot(t, timeout=90):
    log("HARNESS", "Waiting for pyxis boot to complete...")
    line = t.wait_for_line(
        lambda L: "BOOT" in L and "ui_manager" in L and "END" in L,
        timeout,
    )
    if not line:
        return False
    log("HARNESS", f"Boot complete: {line}")
    return True


def wait_for_tcp_link(t, timeout=60):
    log("HARNESS", "Waiting for TCP interface to connect to rnsd...")
    line = t.wait_for_line(
        # Match the actual connection ("Connected to ..."), not interface/worker
        # startup. Matching "started" let the harness proceed (and drive the
        # announce) before the link was up, so the announce was lost and the
        # first direct message failed.
        lambda L: "TCPClientInterface" in L and "connected to" in L.lower(),
        timeout,
    )
    if line:
        log("HARNESS", f"TCP link up: {line}")
    else:
        log("HARNESS", "(TCP-link line not seen — continuing anyway, "
                       "pyxis may have already attached.)")
    return line is not None


def start_echobot(peer_hex=""):
    log("HARNESS", f"Launching echo bot (peer_hex={peer_hex})...")
    fh = open(ECHOBOT_LOG, "w")
    env = dict(os.environ)
    if peer_hex:
        # The bot uses this to proactively request_path() so it picks up
        # pyxis's cached announce from rnsd, even if rnsd's announce
        # rebroadcast limit was already reached when the bot connected.
        env["ECHOBOT_PEER_HEX"] = peer_hex
    proc = subprocess.Popen(
        [BOT_PY, ECHOBOT_PY],
        stdout=fh, stderr=subprocess.STDOUT,
        env=env,
    )
    return proc, fh


def echobot_announce_dest(echobot_log_path, timeout=20):
    """Read the echobot's own log to extract its delivery destination hash."""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            with open(echobot_log_path) as f:
                last = f.read()
        except FileNotFoundError:
            time.sleep(0.5)
            continue
        for line in last.splitlines():
            # `Delivery dest hash=<hex>.`
            if "Delivery dest hash=" in line:
                hex_part = line.split("Delivery dest hash=", 1)[1].rstrip(".").strip()
                # may have trailing punctuation
                hex_part = hex_part.split()[0].rstrip(".")
                return hex_part
        time.sleep(0.5)
    return None


def wait_for_path(t, dest_hex, timeout=120):
    log("HARNESS", f"Polling pyxis for path to bot {dest_hex}...")
    deadline = time.time() + timeout
    last_paths_dump = 0
    while time.time() < deadline:
        resp = t.send_command(f"T:HASPATH {dest_hex}", response_timeout=5)
        if resp and resp.startswith("T:OK 1"):
            log("HARNESS", "Pyxis has path to bot.")
            return True
        # Every 20s, dump T:PATHS to see what pyxis DOES know about
        if time.time() - last_paths_dump > 20:
            last_paths_dump = time.time()
            t.ser.write(b"T:PATHS\n")
            t.ser.flush()
            ok = t.wait_for_line(lambda L: L.startswith("T:OK count="), timeout=5)
            if ok:
                log("HARNESS", f"T:PATHS {ok}")
                # Drain the T:PATH lines that follow
                try:
                    count = int(ok.split("=", 1)[1])
                except Exception:
                    count = 0
                for _ in range(count):
                    p = t.wait_for_line(lambda L: L.startswith("T:PATH "), timeout=2)
                    if p:
                        log("HARNESS", f"   {p}")
        time.sleep(2.0)
    return False


def echobot_log_after(marker, predicate, timeout=30):
    """Wait until the echobot log has a line matching predicate appearing
    after `marker` (a substring known to be present already, used to
    avoid matching old log entries from a previous round)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(ECHOBOT_LOG) as f:
                txt = f.read()
        except FileNotFoundError:
            time.sleep(0.5)
            continue
        if marker:
            mi = txt.find(marker)
            if mi < 0:
                time.sleep(0.5)
                continue
            txt = txt[mi + len(marker):]
        for line in txt.splitlines():
            if predicate(line):
                return line
        time.sleep(0.5)
    return None


def poll_tdeck_rx(t, expected_substring, timeout=60):
    """Poll T:RX until a received message contains expected_substring."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        # T:RX returns multiple lines; we have to read more than one
        log("HARNESS-TX", "T:RX")
        t.ser.write(b"T:RX\n")
        t.ser.flush()
        # Collect: T:OK count=N then N T:RXMSG lines
        ok_line = t.wait_for_line(
            lambda L: L.startswith("T:OK count=") or L.startswith("T:ERR"),
            timeout=5,
        )
        if not ok_line:
            time.sleep(2.0)
            continue
        try:
            count = int(ok_line.split("=", 1)[1])
        except Exception:
            count = 0
        rx_msgs = []
        for _ in range(count):
            line = t.wait_for_line(lambda L: L.startswith("T:RXMSG"), timeout=2)
            if line:
                rx_msgs.append(line)
        for line in rx_msgs:
            if expected_substring in line:
                log("HARNESS", f"Pyxis RX confirmed: {line}")
                return line
        time.sleep(2.0)
    return None


def main():
    global _log_fh
    parser = argparse.ArgumentParser()
    parser.add_argument("--soak-hours", type=float, default=0.0,
                        help="0 = one pass of each method + the bz2 probe (default); "
                             ">0 loops the round-trip tests for a soak")
    parser.add_argument("--cadence", type=float, default=30,
                        help="seconds between message rounds")
    parser.add_argument("--no-reset", action="store_true",
                        help="don't pulse DTR (use if pyxis is already booted)")
    args = parser.parse_args()

    _log_fh = open(HARNESS_LOG, "w")
    log("HARNESS", f"Harness starting; soak target = {args.soak_hours} hours")

    # 1. Open T-Deck serial, reset, wait for boot
    t = TDeck()
    if not args.no_reset:
        t.reset()

    if not wait_for_pyxis_boot(t, timeout=90):
        log("HARNESS", "FAILED: pyxis boot did not complete in 90s")
        t.close()
        return 1

    wait_for_tcp_link(t, timeout=30)
    time.sleep(2.0)

    pyxis_dest_resp = t.send_command("T:DEST", response_timeout=5)
    if not pyxis_dest_resp or not pyxis_dest_resp.startswith("T:OK"):
        log("HARNESS", f"FAILED: T:DEST returned {pyxis_dest_resp}")
        t.close()
        return 1
    pyxis_dest = pyxis_dest_resp.split(" ", 1)[1].strip()
    log("HARNESS", f"Pyxis delivery dest = {pyxis_dest}")

    # 2. Start echo bot AFTER pyxis is up. Pass pyxis's destination
    # hash so the bot can proactively request_path() and pick up
    # the cached announce from rnsd.
    bot, bot_fh = start_echobot(peer_hex=pyxis_dest)
    bot_dest = echobot_announce_dest(ECHOBOT_LOG, timeout=20)
    if not bot_dest:
        log("HARNESS", "FAILED: could not extract bot destination hash")
        bot.terminate()
        t.close()
        return 1
    log("HARNESS", f"Echo bot delivery dest = {bot_dest}")

    # 3. Wait for pyxis to learn the bot's path
    if not wait_for_path(t, bot_dest, timeout=120):
        log("HARNESS", "FAILED: pyxis never learned bot's path")
        bot.terminate()
        t.close()
        return 1

    # 3b. Drive pyxis to announce so the bot learns pyxis's identity
    # for echo construction. Two ways the bot can end up with the
    # identity: (a) live announce delivered through rnsd while the bot
    # is connected — fires our announce_handler and we log "Announce
    # RX" — or (b) a path-response from rnsd's cache when the bot
    # calls RNS.Transport.request_path (which the bot does eagerly
    # for ECHOBOT_PEER_HEX). Path (b) populates Identity.recall but
    # doesn't always fire the announce_handler (path-response
    # delivery is gated on `receive_path_responses` and hits a thread
    # boundary). So treat "bot has pyxis path" OR "bot logged Announce
    # RX" as either-or success. Drive a few T:ANN cycles to give path
    # (a) a chance and not just rely on the eager request_path.
    log("HARNESS", "Driving pyxis to announce, "
                   "waiting for bot to learn pyxis's identity...")
    bot_saw_pyxis = False
    for attempt in range(6):
        t.send_command("T:ANN", response_timeout=5)
        # Either: bot's announce_handler fired (live announce) OR
        # bot's eager request_path landed (path-response → cached
        # announce → Identity.remember). The latter logs "Path to
        # peer <hex> known via cached announce" via the
        # peer_path_acquirer thread.
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                with open(ECHOBOT_LOG) as f:
                    txt = f.read()
            except FileNotFoundError:
                txt = ""
            if (("Announce RX:" in txt and pyxis_dest in txt)
                    or f"Path to peer {pyxis_dest} known" in txt):
                bot_saw_pyxis = True
                break
            time.sleep(0.5)
        if bot_saw_pyxis:
            log("HARNESS", "Bot has pyxis identity (via announce or path-response)")
            break
        log("HARNESS", f"  attempt {attempt+1}: bot not yet — re-announce")
    if not bot_saw_pyxis:
        log("HARNESS", "FAILED: bot never learned pyxis's identity — "
                       "echoes will fail")
        bot.terminate()
        t.close()
        return 1

    fails = 0
    successes = 0

    # 4. bz2-on-receive probe — the graft's riskiest new code. Send a short
    #    "BZ2PROBE" trigger (fits the USB-CDC single-line limit); the echo bot
    #    replies with a ~1.5KB highly-compressible payload. python LXMF sends
    #    that as a bz2-COMPRESSED Resource (auto_compress — microLXMF announces
    #    compression-capable). pyxis must bz2_decompress it in
    #    Resource::assemble() and surface the full body via T:RX. Unpatched
    #    upstream marks a compressed inbound resource CORRUPT and tears down
    #    the link, so a clean decompressed receive here proves the graft's
    #    decompress-on-receive port works on real hardware.
    log("HARNESS", "=== bz2-on-receive probe ===")
    with open(ECHOBOT_LOG) as f:
        probe_marker = f.read()[-256:]
    probe_resp = t.send_command(f"T:SEND {bot_dest} BZ2PROBE", response_timeout=10)
    if probe_resp and probe_resp.startswith("T:OK"):
        bot_rx = echobot_log_after(
            probe_marker,
            lambda L: "Message RX:" in L and "BZ2PROBE" in L,
            timeout=60,
        )
        if not bot_rx:
            log("HARNESS", "FAIL [bz2-probe]: bot never received BZ2PROBE trigger")
            fails += 1
        else:
            rx = poll_tdeck_rx(t, "BZ2OK:", timeout=90)
            if rx and len(rx) > 800:
                log("HARNESS", f"PASS [bz2-probe]: pyxis received + decompressed "
                               f"compressed Resource ({len(rx)}-char T:RXMSG line)")
                successes += 1
            else:
                log("HARNESS", f"FAIL [bz2-probe]: no decompressed payload surfaced "
                               f"(got {rx!r}) — check for link teardown / CORRUPT")
                fails += 1
        t.send_command("T:RXCLR", response_timeout=5)
    else:
        log("HARNESS", f"FAIL [bz2-probe]: T:SEND BZ2PROBE returned {probe_resp!r}")
        fails += 1

    # 5. Round-trip tests. DIRECT + OPPORTUNISTIC always run; PROPAGATED runs
    #    only when PYXIS_PROPAGATION_NODE_HEX is set (deployment-specific hash
    #    kept out of source). Runs one pass minimum, then loops for --soak-hours.
    deadline = time.time() + args.soak_hours * 3600
    round_n = 0
    payloads = [
        ("direct-short",         "T:SEND",    "hi t-deck"),
        ("direct-medium",        "T:SEND",    "this is a 100-char-ish payload to verify pyxis can frame a 1-packet LXMF message cleanly y"),
        ("opportunistic-short",  "T:SENDOPP", "opp t-deck"),
        ("opportunistic-medium", "T:SENDOPP", "opp 100-char payload should fit a single LXMF packet without any link establishment xx"),
    ]

    propagation_node_hex = os.environ.get("PYXIS_PROPAGATION_NODE_HEX", "").strip()
    if propagation_node_hex:
        log("HARNESS", f"Configuring propagation node on pyxis: {propagation_node_hex}")
        set_resp = t.send_command(f"T:SETPROP {propagation_node_hex} 16", response_timeout=5)
        if not set_resp or not set_resp.startswith("T:OK"):
            log("HARNESS", f"WARN: T:SETPROP failed: {set_resp!r}")
        if not wait_for_path(t, propagation_node_hex, timeout=60):
            log("HARNESS", "WARN: pyxis never learned PN path; propagation rounds will fail")
        else:
            log("HARNESS", "Waiting for PN identity to land in recall cache...")
            identity_deadline = time.time() + 60
            while time.time() < identity_deadline:
                r = t.send_command(f"T:RECALL {propagation_node_hex}", response_timeout=5)
                if r and "size=" in r and int(r.split("size=", 1)[1].split()[0]) > 0:
                    log("HARNESS", f"PN identity known: {r}")
                    break
                time.sleep(2.0)
        payloads.append(("propagation-short", "T:SENDPROP", "prop t-deck"))
    else:
        log("HARNESS", "No PYXIS_PROPAGATION_NODE_HEX set — skipping PROPAGATED rounds")

    first_pass = True
    while first_pass or time.time() < deadline:
        first_pass = False
        round_n += 1
        for label, send_cmd, content in payloads:
            log("HARNESS", f"=== Round {round_n} / {label} ===")
            # Mark current echobot log so we only look at lines after this point
            with open(ECHOBOT_LOG) as f:
                marker_text = f.read()
            marker = marker_text[-256:] if len(marker_text) > 256 else marker_text

            # 4a. T-Deck → Bot
            send_resp = t.send_command(f"{send_cmd} {bot_dest} {content}", response_timeout=10)
            if not send_resp or not send_resp.startswith("T:OK"):
                log("HARNESS", f"FAIL [{label}]: T:SEND returned {send_resp}")
                fails += 1
                continue
            # Parse the message hash so we can poll state
            try:
                msg_hash = send_resp.split("hash=", 1)[1].split()[0]
            except Exception:
                msg_hash = None
                log("HARNESS", f"WARN [{label}]: could not parse msg_hash from {send_resp}")

            # Wait for bot to log RX
            rx_line = echobot_log_after(
                marker,
                lambda L: "Message RX:" in L and "from=" + pyxis_dest in L,
                timeout=60,
            )
            if not rx_line:
                log("HARNESS", f"FAIL [{label}]: bot never received from pyxis")
                fails += 1
                continue
            log("HARNESS", f"OK  [{label}]: bot RX: {rx_line.strip()}")

            # Wait for echo back
            echo_line = echobot_log_after(
                marker,
                lambda L: "Echo queued for delivery" in L
                          or "Echo SENT" in L or "Echo DELIVERED" in L,
                timeout=20,
            )
            if not echo_line:
                log("HARNESS", f"WARN [{label}]: bot did not log echo send "
                               f"(may still arrive)")

            # For PROPAGATED rounds the bot's echo doesn't go directly
            # to pyxis — it gets uploaded to the PN first, then pyxis
            # has to sync down. Wait for the bot's "Echo DELIVERED" (or
            # "Echo SENT") log line before kicking a sync, then retry
            # sync a few times because the first sync after bot upload
            # can race the PN's index update.
            if send_cmd == "T:SENDPROP":
                upload_line = echobot_log_after(
                    marker,
                    lambda L: ("Echo DELIVERED" in L
                               or "Echo SENT" in L
                               or "Echo queued" in L),
                    timeout=20,
                )
                # Echo "DELIVERED" for PROPAGATED in python LXMF means
                # uploaded to PN. Even after that the PN index update
                # can lag a few seconds, so we give it a small grace
                # period.
                if upload_line and ("DELIVERED" in upload_line
                                    or "SENT" in upload_line):
                    log("HARNESS", "  Bot uploaded echo to PN; "
                                   "waiting 3s for PN to index it")
                    time.sleep(3)
                else:
                    log("HARNESS", "  WARN: didn't see echo upload; "
                                   "syncing anyway and hoping for the best")

                # Try up to 4 sync rounds. Each call to T:SYNCPROP
                # restarts the FSM if it's idle/complete/failed; we
                # poll T:SYNCSTATE until PR_COMPLETE (=6) or FAILED (=7).
                for sync_attempt in range(4):
                    log("HARNESS",
                        f"  T:SYNCPROP attempt {sync_attempt + 1}/4")
                    t.send_command("T:SYNCPROP", response_timeout=5)
                    sync_deadline = time.time() + 30
                    while time.time() < sync_deadline:
                        sresp = t.send_command("T:SYNCSTATE",
                                               response_timeout=5)
                        if sresp and ("state=6" in sresp
                                      or "state=7" in sresp):
                            break
                        time.sleep(2)
                    # Quick peek for the echo before kicking another sync
                    quick_match = poll_tdeck_rx(t, "echo:", timeout=5)
                    if quick_match:
                        break

            # Wait for pyxis to RX the echo
            rx_timeout = 60 if send_cmd != "T:SENDPROP" else 30
            rx_match = poll_tdeck_rx(t, "echo:", timeout=rx_timeout)
            if not rx_match:
                log("HARNESS", f"FAIL [{label}]: pyxis never received echo")
                fails += 1
                continue
            log("HARNESS", f"OK  [{label}]: pyxis RX echo: {rx_match}")

            # Clear the pyxis RX ring so the next round starts clean
            t.send_command("T:RXCLR", response_timeout=5)

            successes += 1
            log("HARNESS", f"=== Round {round_n} / {label}: PASS "
                           f"(total: {successes} pass, {fails} fail) ===")

        # Stability check: peek for any panic/abort/assert in the T-Deck log
        # (drain new lines but don't block on them).
        for line in t.drain_lines():
            if any(k in line for k in (
                "Guru Meditation", "PANIC", "abort", "assertion", "rst:0x"
            )):
                log("HARNESS", f"CRASH detected: {line}")
                fails += 1
                deadline = 0
                break

        time.sleep(args.cadence)

    log("HARNESS", f"Soak complete. successes={successes} fails={fails}")
    bot.terminate()
    bot_fh.close()
    t.close()
    return 0 if fails == 0 and successes > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
