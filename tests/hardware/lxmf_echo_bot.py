#!/usr/bin/env python3
"""
Mac-side LXMF echo bot for diagnosing pyxis T-Deck delivery issues.

Joins the same RNS network as the T-Deck via AutoInterface (link-local
IPv6 multicast). Announces an identity called "Mac Echo Bot" and echoes
any received DIRECT message back to the sender. Every announce send,
announce receive, message receive, and message send is logged with a
timestamp.

Usage:
    pip install rns lxmf
    python3 /tmp/lxmf_echobot.py

Expected output when working:
    [12:00:00.123] Reticulum up. Identity hash=<hex>. Delivery dest hash=<hex>.
    [12:00:00.234] Sending announce
    [12:00:01.456] Received announce from <hex8>: Some Peer
    [12:00:05.789] Received DIRECT message from <hex8>: 'hello'
    [12:00:05.890] Echoing back: 'echo: hello'
    [12:00:06.012] Echo SENT (state=delivered)
"""
import os, sys, time, threading, datetime

# Pin to a local checkout of RNS + LXMF if one exists, so the bot
# uses the same library pyxis interops with rather than whatever
# pip points at. Override with RNS_REPO / LXMF_REPO env vars.
for env_var, default_subdir in (("RNS_REPO", "repos/Reticulum"),
                                ("LXMF_REPO", "repos/LXMF")):
    p = os.environ.get(env_var) \
        or os.path.expanduser(os.path.join("~", default_subdir))
    if os.path.isdir(p):
        sys.path.insert(0, p)

import RNS
import LXMF


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def log(msg):
    print(f"[{ts()}] {msg}", flush=True)


CONFIG_DIR = "/tmp/echobot-rnsconfig"
STORAGE_DIR = "/tmp/echobot-storage"
DISPLAY_NAME = "Mac Echo Bot"
# rnsd port — must match the one run_e2e.sh bakes into the firmware
# (PYXIS_TEST_TCP_PORT) so the bot and the T-Deck join the same rnsd.
TCP_PORT = os.environ.get("PYXIS_TEST_TCP_PORT", "4242").strip() or "4242"

# Optional: when the harness launches the bot, it sets ECHOBOT_PEER_HEX
# to pyxis's delivery destination hash. We use that to proactively
# request a path / cached-announce so we have pyxis's identity in
# `Identity.recall` cache before the harness drives messages — otherwise
# we depend on rnsd's announce rebroadcast and miss it if rnsd's
# per-announce rebroadcast limit was already reached.
PEER_HEX = os.environ.get("ECHOBOT_PEER_HEX", "").strip()

# lxmd propagation node the bot routes PROPAGATED messages through.
# Deployment-specific, so it comes from PYXIS_PROPAGATION_NODE_HEX (no
# hardcoded default — keeps environment-specific hashes out of source).
# When unset, the bot skips all propagation setup and the harness skips
# its PROPAGATED rounds; DIRECT + OPPORTUNISTIC + the bz2 probe still run.
PROPAGATION_NODE_HEX = os.environ.get("PYXIS_PROPAGATION_NODE_HEX", "").strip()
PROPAGATION_STAMP_COST = 16  # lxmd default is 16; 13 is the floor.

# How often to pull queued messages from the PN. The bot's not running
# any UI, so it relies on this loop to actually receive PROPAGATED
# messages.
PROP_SYNC_INTERVAL_SEC = 8

os.makedirs(CONFIG_DIR, exist_ok=True)
os.makedirs(STORAGE_DIR, exist_ok=True)

# Minimal RNS config: TCPClient to the local rnsd at 127.0.0.1:4242.
# Pyxis on the T-Deck connects as a TCP CLIENT to the same rnsd's
# TCPServerInterface (built into rnsd's default config) so this bot
# and pyxis end up on the same Reticulum network with the rnsd acting
# as a hub.
#
# We use an isolated config dir / non-default control ports so this
# bot does NOT accidentally join the user's primary rnsd shared
# instance (which would mix the bot's identity into their personal
# Reticulum state). The bot is its own RNS process.
config_path = os.path.join(CONFIG_DIR, "config")
with open(config_path, "w") as f:
    f.write(f"""\
[reticulum]
enable_transport = Yes
share_instance = No
shared_instance_port = 47428
instance_control_port = 47429

[logging]
loglevel = 4

[interfaces]
  [[TCP to local rnsd]]
    type = TCPClientInterface
    enabled = yes
    target_host = 127.0.0.1
    target_port = {TCP_PORT}
""")

reticulum = RNS.Reticulum(configdir=CONFIG_DIR, loglevel=4)

# Persistent identity stored in the config dir so reruns reuse the
# same delivery destination hash (peers don't have to re-learn the
# path on every restart).
ident_path = os.path.join(CONFIG_DIR, "identity")
if os.path.exists(ident_path):
    identity = RNS.Identity.from_file(ident_path)
    log(f"Loaded identity from {ident_path}")
else:
    identity = RNS.Identity()
    identity.to_file(ident_path)
    log(f"Created new identity, saved to {ident_path}")

router = LXMF.LXMRouter(identity=identity, storagepath=STORAGE_DIR)
delivery_destination = router.register_delivery_identity(
    identity, display_name=DISPLAY_NAME
)
delivery_destination.set_default_app_data(
    lambda: router.get_announce_app_data(delivery_destination.hash)
)

log(f"Reticulum up. Identity hash={identity.hash.hex()}.")
log(f"Delivery dest hash={delivery_destination.hash.hex()}.")
log(f"Display name: {DISPLAY_NAME}")

# Configure propagation node so the bot can SEND propagated and SYNC
# its inbox. We have to wait for the PN to be path-known before
# `set_outbound_propagation_node` is useful; the path arrives via
# the rnsd<->lxmd shared instance. Set it right away anyway —
# request_messages_from_propagation_node tolerates "no path yet"
# and retries the path request internally.
prop_node_hash = bytes.fromhex(PROPAGATION_NODE_HEX) if PROPAGATION_NODE_HEX else None
if prop_node_hash is not None:
    router.set_outbound_propagation_node(prop_node_hash)
    router.outbound_propagation_node = prop_node_hash
    log(f"Configured outbound propagation node: {PROPAGATION_NODE_HEX}")
else:
    log("No PYXIS_PROPAGATION_NODE_HEX set — propagation disabled (DIRECT/OPP/bz2 only)")


def on_announce(destination_hash, announced_identity, app_data):
    name = LXMF.display_name_from_app_data(app_data) or "(no name)"
    log(f"Announce RX: dest={destination_hash.hex()} name={name!r}")


announce_handler = type(
    "Handler", (), {
        "aspect_filter": "lxmf.delivery",
        # Without `receive_path_responses = True`, RNS only fires this
        # handler for LIVE announces (received via broadcast/forward).
        # Path responses (the cached announce sent back by a next-hop
        # in reply to RNS.Transport.request_path) are skipped. The
        # harness's eager-path-acquirer relies on path responses to
        # learn pyxis's identity quickly, so we opt in here.
        "receive_path_responses": True,
        "received_announce": staticmethod(on_announce),
    }
)
RNS.Transport.register_announce_handler(announce_handler)


def on_delivery(message):
    src_hex = message.source_hash.hex() if message.source_hash else "?"
    method = {
        LXMF.LXMessage.OPPORTUNISTIC: "OPPORTUNISTIC",
        LXMF.LXMessage.DIRECT: "DIRECT",
        LXMF.LXMessage.PROPAGATED: "PROPAGATED",
    }.get(message.method, str(message.method))
    try:
        content = message.content.decode("utf-8")
    except Exception:
        content = repr(message.content)
    title = ""
    try:
        if message.title:
            title = message.title.decode("utf-8")
    except Exception:
        title = repr(message.title)
    log(
        f"Message RX: from={src_hex} method={method} "
        f"title={title!r} content={content!r}"
    )

    # Echo back. Mirror the sender's method so PROPAGATED messages
    # echo via the PN (round-trip: sender uploads → PN persists → bot
    # syncs down → bot replies via PN → sender syncs down) and DIRECT
    # messages echo over a fresh link.
    try:
        # Recall the sender's identity so we can construct a destination.
        sender_identity = RNS.Identity.recall(message.source_hash)
        if sender_identity is None:
            log(f"  Cannot echo: sender identity not yet known for {src_hex}")
            return
        dest = RNS.Destination(
            sender_identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
            "lxmf", "delivery"
        )
        echo_method = (
            LXMF.LXMessage.PROPAGATED
            if message.method == LXMF.LXMessage.PROPAGATED
            else LXMF.LXMessage.DIRECT
        )
        # bz2-on-receive probe: when the harness sends "BZ2PROBE", reply with
        # a large, highly-compressible payload. It exceeds microLXMF's Resource
        # threshold AND compresses ~99%, so python LXMF sends it as a bz2-
        # compressed Resource (auto_compress, since microLXMF announces
        # compression-capable). That forces pyxis's Resource::assemble() to
        # bz2_decompress on receive — the path that, unpatched upstream, marks
        # the resource CORRUPT and tears down the link. If pyxis surfaces the
        # full payload via T:RX, decompress-on-receive works on hardware.
        if content.startswith("BZ2PROBE"):
            echo_body = "BZ2OK:" + ("ABCDABCDABCDABCD" * 96)  # ~1542 bytes, repeating
        else:
            echo_body = f"echo: {content}"
        echo_kwargs = dict(
            destination=dest,
            source=delivery_destination,
            content=echo_body.encode("utf-8"),
            title=b"echo",
            desired_method=echo_method,
        )
        if echo_method == LXMF.LXMessage.PROPAGATED:
            # python LXMF requires `include_ticket=False` here so the
            # outbound propagation flow doesn't trip on missing ticket
            # state. The default is fine but spelled out for clarity.
            echo_kwargs["include_ticket"] = False
        echo = LXMF.LXMessage(**echo_kwargs)

        def on_delivered(msg):
            log(f"  Echo DELIVERED to {src_hex}")
        def on_failed(msg):
            log(f"  Echo FAILED to {src_hex} state={msg.state}")
        def on_sent(msg):
            log(f"  Echo SENT (PRF received) to {src_hex}")

        echo.register_delivery_callback(on_delivered)
        echo.register_failed_callback(on_failed)
        # `sent` only fires on PROPAGATED in upstream LXMF; we still set it
        # for parity with the `delivery` callback nomenclature.
        try:
            echo.register_sent_callback(on_sent)
        except Exception:
            pass

        router.handle_outbound(echo)
        log(f"  Echo queued for delivery to {src_hex}")
    except Exception as e:
        log(f"  Echo construction failed: {e}")


router.register_delivery_callback(on_delivery)


# Announce loop — every 30s so the T-Deck hears us multiple times.
def announcer():
    while True:
        delivery_destination.announce()
        log("Announce TX")
        time.sleep(30)


# Propagation sync loop — periodically pull queued messages from the PN.
# Without this the bot would never receive PROPAGATED messages (no UI
# tap to drive a manual sync; the LXMRouter only auto-syncs when its
# `delivery_destination` has someone tapping `sync_inbound`).
#
# lxmd's default propagation announce_interval is 5 MINUTES (the value
# in the config is multiplied by 60), so waiting passively for an
# announce can stall the first sync several minutes. We proactively
# `RNS.Transport.request_path()` until we get a path.
def prop_syncer():
    # Bounded path acquisition: give up after ~60 request rounds (~5 min) rather
    # than spinning forever if the PN is unreachable. The PROPAGATED rounds just
    # won't run in that case; DIRECT/OPP/bz2 are unaffected.
    for _attempt in range(60):
        if RNS.Transport.has_path(prop_node_hash):
            break
        log("  Requesting path to PN...")
        RNS.Transport.request_path(prop_node_hash)
        for _ in range(10):
            if RNS.Transport.has_path(prop_node_hash):
                break
            time.sleep(0.5)
    if not RNS.Transport.has_path(prop_node_hash):
        log("  WARN: no path to PN after retries; propagation sync disabled")
        return
    log(f"Path to PN known; starting periodic sync every {PROP_SYNC_INTERVAL_SEC}s")
    while True:
        try:
            router.request_messages_from_propagation_node(identity)
        except Exception as e:
            log(f"  prop sync error: {e}")
        time.sleep(PROP_SYNC_INTERVAL_SEC)


threading.Thread(target=announcer, daemon=True).start()
if prop_node_hash is not None:
    threading.Thread(target=prop_syncer, daemon=True).start()


# Eager peer-path acquisition. When the harness tells us pyxis's hash
# via env, request a path immediately. RNS forwards the cached announce
# from the next-hop, which fires our announce-handler and populates
# Identity.recall_app_data. Without this, if the bot connects AFTER
# rnsd hit its rebroadcast limit for the peer's announce, we never
# learn pyxis's identity and can't echo PROPAGATED messages.
if PEER_HEX:
    def peer_path_acquirer():
        try:
            peer_hash = bytes.fromhex(PEER_HEX)
        except Exception as e:
            log(f"  Bad ECHOBOT_PEER_HEX={PEER_HEX!r}: {e}")
            return
        for _ in range(10):
            if RNS.Transport.has_path(peer_hash):
                log(f"Path to peer {PEER_HEX} known via cached announce")
                return
            log(f"  Requesting path to peer {PEER_HEX}...")
            RNS.Transport.request_path(peer_hash)
            for _ in range(20):
                if RNS.Transport.has_path(peer_hash):
                    log(f"Path to peer {PEER_HEX} known via cached announce")
                    return
                time.sleep(0.5)
        log(f"  WARN: never got path to peer {PEER_HEX}")

    threading.Thread(target=peer_path_acquirer, daemon=True).start()
log(f"Echo bot ready. Press Ctrl-C to stop. Storage: {STORAGE_DIR}")

try:
    while True:
        time.sleep(60)
except KeyboardInterrupt:
    log("Shutting down.")
