# Pyxis Tests

Three test surfaces, each runnable independently.

## 1. Pyxis-unique pytest suite

Native C++ tests of pyxis-unique code (BLE fragmenter, HDLC, etc.) and Python tests of build scripts.

```bash
/usr/bin/python3 -m pytest tests/build_scripts tests/native -v
```

System Python 3.9 has pytest pre-installed; Homebrew Python does not.

- `build_scripts/test_patch_nimble.py` — verifies `patch_nimble.py` idempotency, drift detection, missing-file handling
- `native/test_hdlc.{cpp,py}` — HDLC escape/unescape/frame round-trip + golden vector against Python RNS
- `native/test_ble_fragmenter.{cpp,py}` — BLEFragmenter ↔ BLEReassembler: in-order, out-of-order, duplicate, dropped+timeout, per-peer isolation, MTU change
- `native/test_ble_peer_manager.{cpp,py}` — connection-map state machine: discover, identity promotion, blacklist, handle map cleanup, MAC rotation, pool exhaustion
- `native/test_ble_operation_queue.{cpp,py}` — GATT op queue: FIFO, busy-state, timeout, clearForConnection, builder
- `native/test_ring_buffers.{cpp,py}` — PCM + encoded SPSC ring buffers, including 100k-frame multithreaded producer/consumer stress
- `native/test_audio_filters.{cpp,py}` — VoiceFilterChain frequency response, peak limiting, multichannel

### Adding a new native C++ test

Pattern: pure C++ unit + thin pytest wrapper.

1. Write `tests/native/test_<thing>.cpp` — include from `../../{src,lib/...}` for the unit under test, use the existing `EXPECT_EQ`/`EXPECT_TRUE`/`RUN(name)` framework, return non-zero on any failure.
2. If the unit pulls in microReticulum types, the shims in `tests/native/` cover what's been needed so far:
   - `Bytes.h` → `bytes_shim.h` (minimal `RNS::Bytes` — append/data/size/writable/resize/mid)
   - `Log.h` (no-op `TRACE`/`WARNING`/`INFO`/`ERROR` macros)
   - `Utilities/OS.h` (`OS::time()` with `set_fake_time()`/`clear_fake_time()`)
3. Write `tests/native/test_<thing>.py` — copy the `test_hdlc.py` template, swap source/include paths, run.
4. Confirm: `/usr/bin/python3 -m pytest tests/native/test_<thing>.py -v`

## 2. LXST audio interop tests

Python tests verifying wire format and codec compatibility between pyxis (C++), Python LXST, and LXST-kt (Kotlin).

```bash
/usr/bin/python3 -m pytest tests/interop -v
```

Requires `pycodec2` and access to `$HOME/repos/public/LXST`.

## 3. microReticulum native unit tests (PlatformIO)

The fork's microReticulum tests live under `deps/microReticulum/test/` and run via PlatformIO Unity.

```bash
cd deps/microReticulum && pio test -e native17
```

Use `native17`, not `native` — the C++11 env is broken (static-constexpr ODR-use). Baseline as of 2026-05-02: 94/114 PASS, 7 FAIL, 5 SKIPPED, 7 suites ERRORED. The clean suites are
`test_os, test_bytes, test_msgpack, test_crypto, test_filesystem, test_objects, test_interop, test_general, test_reference, test_example, test_collections`.
