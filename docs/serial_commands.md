# Pyxis serial commands

USB-CDC-driven test/debug command surface, gated behind `-DPYXIS_TEST_HOOKS`
in `platformio.ini`. All commands and responses live on the same line-oriented
serial channel as the regular log stream — host scripts are expected to skip
non-`T:` lines while reading replies.

## Convention

- **Request:** `T:<NAME>[ <args>]\n` — sent to pyxis on the USB-CDC port (115200 8N1).
- **Reply:** every command emits exactly one of:
  - `T:OK [<payload>]` — success
  - `T:ERR <reason>` — failure
- A few commands stream multiple lines (e.g. `T:PATHS`, `T:RX`, `T:SCREENSHOT`).
  Each emits `T:OK` first with a count, then per-item lines, then nothing more
  until the next request.
- Hex args are unprefixed (`abcdef…`) and accepted at full length unless
  otherwise noted (most are 16 bytes / 32 hex chars for LXMF destination hashes).

If `T:ERR unknown cmd <X>` comes back, the build either lacks `PYXIS_TEST_HOOKS`
or `<X>` is misspelled.

## Commands

### Identity, addresses, paths

| Command | Args | Reply | Notes |
|---|---|---|---|
| `T:ID` | — | `T:OK <hex>` | Local Reticulum identity hash (32 hex). |
| `T:DEST` | — | `T:OK <hex>` | LXMF delivery destination hash (16 hex). |
| `T:LXSTDEST` | — | `T:OK <hex>` | LXST telephony destination hash. Used to share with the test bot before `T:CALL`. |
| `T:ANN` | — | `T:OK announced` | Force a fresh LXMF delivery announce. |
| `T:ANNLXST` | — | `T:OK announced` | Force a fresh LXST telephony announce. Required before pyxis-as-callee tests because the TCP-reconnect path only re-announces LXMF. |
| `T:PATHS` | — | `T:OK count=N` then `T:PATH <hex>` per row | Dump the in-memory path table. |
| `T:HASPATH` | `<hex>` | `T:OK 0/1 mem=0/1 mem_count=N` | Has-path check + diagnostic split between disk-backed `Transport::has_path` and the in-memory `_path_table`. |
| `T:RECALL` | `<hex>` | `T:OK <hex>` or `T:ERR not recallable` | Try to resolve `<hex>` to its identity hash via `Identity::recall`. |
| `T:HASIDENTITY` | `<hex>` | `T:OK 0/1` | Boolean check whether pyxis has a recallable identity for `<hex>`. |

### Send / receive

| Command | Args | Reply | Notes |
|---|---|---|---|
| `T:SEND` | `<hex_dest> <text>` | `T:OK <hash>` | Send LXMF message via DIRECT (link). Reply is the message hash. |
| `T:SENDOPP` | `<hex_dest> <text>` | `T:OK <hash>` | Same but OPPORTUNISTIC (single packet). |
| `T:STATE` | `<msg_hash>` | `T:OK <state>` | Outbound message state: `OUTBOUND` / `SENDING` / `SENT` / `DELIVERED` / `FAILED`. |
| `T:RX` | — | `T:OK count=N` then `T:RX <from_hash> <text>` per message | Drain the test-side inbox of received messages (see also `test_rx_count`). |
| `T:RXCLR` | — | `T:OK cleared` | Reset the test-side inbox counters. |

### Propagation (offline-tolerant delivery)

| Command | Args | Reply | Notes |
|---|---|---|---|
| `T:SETPROP` | `<hex_dest> <cost>` | `T:OK` | Configure outbound propagation node hash + LXMF stamp cost. |
| `T:SENDPROP` | `<hex_dest> <text>` | `T:OK <hash>` | Send PROPAGATED via the configured propagation node. |
| `T:SYNCPROP` | — | `T:OK` | Kick off an inbound sync from the configured propagation node. |
| `T:SYNCSTATE` | — | `T:OK <state>` | Current `PR_*` state of the propagation-sync FSM. |

### Voice / LXST telephony

| Command | Args | Reply | Notes |
|---|---|---|---|
| `T:CALL` | `<hex_dest>` | `T:OK` or `T:ERR` | Initiate outgoing LXST call. |
| `T:CALL_ANSWER` | — | `T:OK` | Accept an incoming ring. Only valid in state `RING`. |
| `T:CALL_HANGUP` | — | `T:OK` | Tear down the active call. |
| `T:CALL_STATE` | — | `T:OK <state>` | Current call FSM state name. |
| `T:CALL_STATS` | — | `T:OK …` | Audio frame counters for the most recent call. |
| `T:CALL_QOS` | — | `T:OK …` | Wire-level audio fidelity counters (decoded RMS, frame loss, etc). |
| `T:CALL_PROFILE` | `[hex]` | `T:OK <hex>` | Get (no arg) or set (hex arg) preferred Codec2 profile. Profiles: `0x10` ULBW (700C), `0x20` VLBW (1600), `0x30` LBW (3200). |
| `T:CALL_INJECT` | `<on\|off> [freq_hz] [amp_pct]` | `T:OK inject=<on/off> freq=<f> amp=<a>` | Replace mic capture with a synthesized sine wave for the active call. Useful for end-to-end audio fidelity checks against a bot that decodes pyxis's audio frames. Defaults: 1000 Hz, amp 0.5. |

### BLE interface

| Command | Args | Reply | Notes |
|---|---|---|---|
| `T:BLE` | `on\|off` | `T:OK ble=<on/off>` | Toggle the BLE Mesh interface at runtime AND persist the setting (NVS namespace `settings`, key `ble_en`). |

### UI / docs

| Command | Args | Reply | Notes |
|---|---|---|---|
| `T:SHOW` | `<screen>` | `T:OK shown <screen>` | Switch the active LVGL screen. Names: `conversation_list` (alias `home`), `compose`, `announces`, `status`, `settings`, `propagation_nodes`. |
| `T:SCREENSHOT` | — | Multi-line; see below | Capture the active screen as RGB565 and base64-dump it. |

#### `T:SCREENSHOT` wire format

```
T:SCREENSHOT BEGIN W=320 H=240 FMT=rgb565be BYTES=153600
<base64 line, ≤76 chars>
<base64 line>
…
T:SCREENSHOT END
```

`FMT` carries the byte order — `rgb565be` when `LV_COLOR_16_SWAP=1` is set in
`lib/lv_conf.h` (the current default for the ST7789 panel), `rgb565le`
otherwise. The host script (`screenshot.py`) reads until `END`, filters out
any interleaved log lines (heap heartbeats, BLE stats), validates the byte
count matches the header, and writes a PNG.

Throughput: ~205 KB base64 over CDC at 115200 baud → roughly 18s per shot.
Bumping baud or compressing on-device (zlib via the vendored bz2 path) is the
obvious win if this becomes a hot path.

## Host-side helpers

- **`screenshot.py`** — auto-detects the pyxis port, sends `T:SCREENSHOT`,
  decodes RGB565, writes a PNG. `--port` / `--out` flags. See file header for
  full usage.

(More host helpers welcome — keep them thin wrappers around `T:` so the
device-side surface stays the source of truth.)

## Adding a new command

1. New `else if (cmd == "T:NEWTHING") { … }` block under
   `process_test_command` in `src/main.cpp` (gated behind `PYXIS_TEST_HOOKS`).
2. Reply with exactly one terminal line — `T:OK <payload>` on success or
   `T:ERR <reason>` on failure. Multi-line streams should emit a
   `T:OK count=N` header so the host knows how many follow-up lines to read.
3. Document the command in this file under the right section, and link from
   any host script that drives it.
