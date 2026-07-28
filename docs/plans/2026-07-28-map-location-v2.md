# Map Display and Location Sharing V2 Implementation Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Add a responsive offline-first map and opt-in Sideband/Columba-compatible peer location sharing to current Pyxis without regressing heap, watchdog, storage, messaging, or UI behavior.

**Architecture:** Build the feature as portable C++17 cores first: a bounded telemetry codec, fixed-capacity location/session state, deterministic persistence records, and Web Mercator/viewport math. Keep LXMF, GPS, LittleFS/SD, download transport, and LVGL behind narrow adapters so nearly all behavior runs under x86 tests and sanitizers. Integrate one layer at a time; do not port the historical branch wholesale.

**Tech Stack:** C++17, pytest-driven native executables, ASan/UBSan, LXMF fields, Sideband Telemeter MessagePack, LittleFS, SD, LVGL 8.3, ESP32-S3/FreeRTOS, PlatformIO `tdeck`.

---

## 1. Fixed constraints and source baseline

- Worktree: `/home/tyler/repos/pyxis-map-location-v2`
- Branch: `feature/map-location-v2`
- Base: `origin/main` at `01e8694c4a0a226b197fbd8f271a8127778c3138`
- Historical reference only: `origin/feature/map-telemetry` at `8518a5d1b34a366afec7cc09e31bf8e46bbc5eb3`
- Do not change the pinned microReticulum or microLXMF revisions unless a separately justified protocol defect requires a coordinated update.
- Never format or erase NVS, LittleFS, SD, identities, settings, messages, or conversations.
- No push, PR, release, package publication, or device flash without explicit authorization.
- Every production behavior starts with a failing focused test. Observe RED, then implement GREEN, then run focused and full host suites.
- Hardware-only claims remain pending until a T-Deck is actually exercised.

## 2. Why this is a clean rebuild

The prototype is 127 commits behind current `main` and conflicts in microReticulum, `UIManager.cpp`, `UIManager.h`, and `src/main.cpp`. Its wire contract is also stale:

- It defines Columba metadata as field `0x70`; current Columba/iOS use LXMF `FIELD_CUSTOM_META = 0xFD`.
- It emits JSON metadata; the current canonical form is MessagePack `{cease?, expires?, approxRadius?, ts?}`.
- It writes SPIFFS directly, while current Pyxis mounts LittleFS and must preserve mount failures without formatting.
- Its parser has unchecked or weakly checked lengths and does not robustly skip unknown MessagePack values.
- It couples protocol, persistence, downloads, map rendering, and UI integration.

Use the old branch for UX ideas and behavioral cases only. Do not cherry-pick its implementation commits.

## 3. Automated-test hierarchy

Each layer must pass the strongest applicable gate before the next layer begins:

1. **Portable x86 unit test:** production C++ compiled by `g++`/`clang++`, driven by pytest.
2. **ASan/UBSan native test:** malformed inputs, limits, lifecycle, and stress cases.
3. **Committed cross-language vectors:** decode canonical Sideband/Columba bytes and emit exact canonical bytes where canonical ordering is defined.
4. **Reference implementation check:** optional local Sideband `Telemeter.from_packed()` and current Columba codec comparison when their checkouts/toolchains are available.
5. **Full Pyxis host suite:** `pytest tests/build_scripts tests/native -q`.
6. **microReticulum native17 suites:** only when LXMF/Reticulum integration changes require them; preserve current pinned revision.
7. **Exact firmware compilation:** `pio run -e tdeck`.
8. **Artifact inspection:** size, embedded version, dependency revisions, and SHA-256.
9. **Physical T-Deck acceptance:** heap/stack/watchdog, UI latency, storage persistence, first and second reboot, and real-peer Sideband/Columba behavior.

Baseline observed before feature code: `30 passed in 4.91s` for `tests/build_scripts` + `tests/native`. PlatformIO is not currently on PATH and must be provisioned before the firmware-build gate.

## 4. Proposed production layout

Portable code must avoid `Arduino.h`, `String`, LVGL, FreeRTOS, and filesystem headers.

- `lib/tdeck_ui/Telemetry/LocationTelemetry.h`
  - Plain value types, field IDs, validity/error enums, explicit units.
- `lib/tdeck_ui/Telemetry/LocationTelemetryCodec.h`
- `lib/tdeck_ui/Telemetry/LocationTelemetryCodec.cpp`
  - Bounded MessagePack encoder/decoder over caller-owned buffers.
- `lib/tdeck_ui/Telemetry/LocationShareState.h`
- `lib/tdeck_ui/Telemetry/LocationShareState.cpp`
  - Fixed-capacity peer locations and outbound sessions; deterministic eviction.
- `lib/tdeck_ui/Telemetry/LocationStateRecord.h`
- `lib/tdeck_ui/Telemetry/LocationStateRecord.cpp`
  - Versioned, endian-stable, CRC-protected persistence record encoding/decoding.
- `lib/tdeck_ui/UI/LXMF/MapProjection.h`
- `lib/tdeck_ui/UI/LXMF/MapProjection.cpp`
  - Pure Web Mercator and viewport/tile coverage math.
- `lib/tdeck_ui/Hardware/TDeck/MapTileStore.h/.cpp`
  - SD-backed bounded cache/index adapter, added only after core map behavior is proven.
- `lib/tdeck_ui/Hardware/TDeck/MapTileFetcher.h/.cpp`
  - Optional bounded network fetch adapter, added last.
- `lib/tdeck_ui/UI/LXMF/MapScreen.h/.cpp`
  - LVGL-only presentation using stable reusable objects.
- `lib/tdeck_ui/Telemetry/LocationPersistenceLittleFS.h/.cpp`
  - Small Arduino adapter for transactional state files; no host logic.

Modify `lib/tdeck_ui/library.json` only when a new production `.cpp` directory must enter the firmware build.

## 5. Task plan

### Task 0: Freeze baseline and add native-test helper

**Objective:** Make all new portable tests use one strict compile/run path, including sanitizers where supported.

**Files:**
- Create: `tests/native/native_test.py`
- Test: migrate no existing tests initially; add self-test through first feature wrapper.

**Steps:**
1. Record branch, base SHA, dirty state, dependency pins, and baseline test output.
2. Add a helper that selects `clang++` or `g++`, compiles C++17 with `-Wall -Wextra -Werror -pedantic`, and optionally adds `-fsanitize=address,undefined -fno-omit-frame-pointer`.
3. Make it return compiler stdout/stderr and execute with a timeout.
4. Use the helper from the first location test; do not churn unrelated native tests in this task.
5. Run `pytest tests/build_scripts tests/native -q`.
6. Commit: `test: add strict native test harness`.

### Task 1: Pin canonical telemetry fixtures

**Objective:** Establish the wire contract independently of the implementation.

**Files:**
- Create: `tests/fixtures/location_telemetry_vectors.json`
- Create: `tests/reference/generate_location_telemetry_vectors.py`
- Create: `tests/reference/test_location_telemetry_vectors.py`

**Required vectors:**
- Positive and negative latitude/longitude.
- Negative altitude.
- Zero and non-zero speed/bearing.
- Accuracy `0`, ordinary value, and `>655.35 m` clamp.
- Timestamps encoded as positive fixint, uint8, uint16, uint32, and uint64 where protocol-valid.
- Map entries in both key orders.
- Unknown sensor before and after location.
- Canonical metadata: cease, expires, approximate radius, millisecond timestamp, and combinations.
- Malformed/truncated arrays, wrong binary lengths, invalid coordinate ranges, NaN/Inf source inputs, oversized maps/containers, and trailing data.

**Steps:**
1. Generate canonical Telemeter fixtures from a pinned Sideband checkout when available; record its SHA in the fixture metadata.
2. Cross-check representative vectors against current Columba `TelemeterCodec.kt` and iOS `ColumbaMetaCodec.swift` semantics.
3. Keep generated bytes committed so CI does not require Sideband or Android toolchains.
4. Add a Python schema/integrity test for fixture completeness and unique names.
5. Run the fixture test and full host suite.
6. Commit: `test: pin location telemetry reference vectors`.

### Task 2: Implement bounded telemetry decode using strict TDD

**Objective:** Decode valid Sideband/Columba telemetry and reject malformed or unsafe input without allocation or partial-state mutation.

**Files:**
- Create: `lib/tdeck_ui/Telemetry/LocationTelemetry.h`
- Create: `lib/tdeck_ui/Telemetry/LocationTelemetryCodec.h`
- Create: `lib/tdeck_ui/Telemetry/LocationTelemetryCodec.cpp`
- Create: `tests/native/test_location_telemetry_codec.cpp`
- Create: `tests/native/test_location_telemetry_codec.py`

**Wished-for API:**

```cpp
Telemetry::DecodeResult decodeLocationTelemetry(
    const uint8_t* data,
    size_t size,
    Telemetry::LocationTelemetry& output);
```

**Behavior:**
- Output changes only on success.
- Accept protocol-valid integer widths and map key order.
- Require seven location elements and exact binary widths: 4/4/4/4/4/2 bytes.
- Skip unknown values with bounded depth and bounded aggregate item count.
- Reject overflow, truncation, invalid MessagePack types, duplicate location ambiguity, non-finite/out-of-range coordinates, impossible negative unsigned values, excessive nesting, and oversized payloads.
- Treat `last_update` as authoritative location timestamp; preserve outer sensor timestamp separately if useful.

**TDD cycle per behavior:**
1. Add one failing vector assertion.
2. Run only `pytest tests/native/test_location_telemetry_codec.py -q`; verify expected RED.
3. Add minimal production logic.
4. Re-run focused test to GREEN.
5. Run sanitizer mode and full host suite.
6. Refactor only while green.
7. Commit: `feat: decode bounded location telemetry`.

### Task 3: Implement canonical telemetry encode using strict TDD

**Objective:** Emit Sideband-compatible Telemeter bytes without heap allocation.

**Files:** same codec files and tests as Task 2.

**Wished-for API:**

```cpp
Telemetry::EncodeResult encodeLocationTelemetry(
    const Telemetry::LocationTelemetry& input,
    uint8_t* output,
    size_t capacity,
    size_t& written);
```

**Behavior:**
- Caller-owned fixed buffer; explicit `BUFFER_TOO_SMALL` result.
- Big-endian fixed-point fields with documented rounding/truncation selected from the pinned Sideband reference.
- Clamp speed to non-negative and accuracy to unsigned 16-bit range.
- Reject invalid latitude, longitude, bearing policy violations, non-finite values, and timestamps that cannot be represented by the selected wire contract.
- Compare exact bytes for canonical fixture inputs.

**Verification:** focused RED/GREEN, ASan/UBSan, all native tests, fixture reference decoder acceptance.

**Commit:** `feat: encode canonical location telemetry`.

### Task 4: Implement `FIELD_CUSTOM_META` codec

**Objective:** Correctly encode/decode current Columba metadata at LXMF field `0xFD`.

**Files:** codec production and native test files.

**Behavior:**
- Constants: `FIELD_TELEMETRY = 0x02`, `FIELD_ICON_APPEARANCE = 0x04`, `FIELD_CUSTOM_META = 0xFD`.
- MessagePack map, never JSON for new outbound messages.
- Optional keys: `cease`, `expires`, `approxRadius`, `ts`.
- Unknown keys skipped safely.
- Absent, malformed, false, and true cease values remain distinct.
- Empty metadata is omitted outbound.
- Cease frame requires both a valid zeroed Telemeter body and `{ "cease": true }` metadata for current Columba interoperability.

**Tests:** canonical byte vectors, key reordering, integer widths, malformed values, JSON legacy input explicitly rejected or separately compatibility-gated by a documented test.

**Commit:** `feat: support Columba location metadata`.

### Task 5: Implement fixed-capacity peer location state

**Objective:** Maintain peer-controlled location state deterministically with no unbounded vectors.

**Files:**
- Create: `lib/tdeck_ui/Telemetry/LocationShareState.h/.cpp`
- Create: `tests/native/test_location_share_state.cpp/.py`

**Behavior:**
- Fixed maximum peer count chosen from measured UI/memory needs (initial target: 32).
- Update in place for existing peer.
- Reject stale timestamps unless an explicit cease is newer/equivalent under defined rules.
- Remove exact peer on cease; unrelated peers unchanged.
- Reuse vacant slots before eviction.
- Deterministic oldest-received eviction with stable tie-breaker.
- Enforce expiry and configurable stale-display age.
- Snapshot API copies into caller-owned storage; no UI access to mutable internal containers.

**Tests:** insert/update, capacity, reuse, eviction, stale rejection, timestamp wrap policy, expiry boundary `t-1/t/t+1`, cease ordering, unrelated peer isolation, 100k-operation sanitizer stress.

**Commit:** `feat: add bounded peer location state`.

### Task 6: Implement outbound sharing sessions and privacy state

**Objective:** Model explicit per-peer consent, cadence, expiry, cease, and reboot semantics without network or UI dependencies.

**Files:** same state files/tests.

**Behavior:**
- Fixed-capacity sessions.
- Durations: 15 minutes, 1 hour, 4 hours, local midnight, indefinite.
- Starting/updating a session requests immediate send but does not mark it sent.
- Scheduler returns work; caller acknowledges successful queueing before advancing `last_sent`.
- Failed sends retry with bounded backoff and never silently extend expiry.
- Expiration schedules one cease frame and then removes the session only after queue acceptance or explicit local cancellation policy.
- Privacy default is not sharing.
- No automatic resume after corrupted state.
- Define whether sessions resume after reboot; safest initial policy is restore consent/expiry but require current valid GPS before sending and immediately remove already-expired sessions.

**Tests:** all duration boundaries, midnight calculation, clock unavailable/backward jumps, queue failure, successful acknowledgement, stop/cease lifecycle, capacity, reboot reconstruction.

**Commit:** `feat: add location sharing scheduler`.

### Task 7: Implement versioned transactional persistence records

**Objective:** Persist sessions and received locations without raw-struct dumps or filesystem-specific host code.

**Files:**
- Create: `lib/tdeck_ui/Telemetry/LocationStateRecord.h/.cpp`
- Create: `tests/native/test_location_state_record.cpp/.py`
- Later create: `lib/tdeck_ui/Telemetry/LocationPersistenceLittleFS.h/.cpp`

**Record contract:** magic, schema version, payload length, explicit endian encoding, bounded counts, records, CRC32. Never persist compiler padding, enums by native representation, pointers, or raw doubles without an explicit stable representation.

**Tests:** golden bytes, round-trip, version mismatch, truncation at every byte, count overflow, CRC corruption, duplicate peers, invalid coordinates, temporary/live/backup recovery decision table.

**LittleFS adapter behavior:** write temp, flush/close, read and validate temp, preserve validated backup, rename, validate live; on any failure retain a recoverable generation. Mount failure reports unavailable and never formats.

**Commit sequence:**
- `feat: encode persistent location state`
- `feat: persist location state transactionally`

### Task 8: Add pure Web Mercator and viewport coverage math

**Objective:** Prove all map coordinate and tile-selection behavior on x86 before LVGL exists.

**Files:**
- Create: `lib/tdeck_ui/UI/LXMF/MapProjection.h/.cpp`
- Create: `tests/native/test_map_projection.cpp/.py`

**Behavior:**
- Clamp latitude to Web Mercator limit ±85.05112878°.
- Normalize longitude and wrap tile X across the antimeridian.
- Clamp tile Y and validate zoom without `1 << z` overflow.
- Lat/lon ↔ global pixel conversions with explicit tile size.
- Compute every tile intersecting a viewport plus optional one-tile prefetch border.
- A 320-pixel viewport can span three 256-pixel tiles at arbitrary offsets; tests must prevent the old fixed-2×2 bug.
- Stable marker projection and off-screen clipping.

**Tests:** equator/prime meridian, world corners, poles, antimeridian, zoom min/max, round trips, viewport edge alignment, 2/3-tile transitions, pan deltas, property/fuzz loops, sanitizer stress.

**Commit:** `feat: add tested map projection core`.

### Task 9: Add an LXMF location-message classifier seam

**Objective:** Keep telemetry-only messages out of chat persistence/notifications while preserving mixed text+telemetry messages.

**Files:**
- Create: `lib/tdeck_ui/Telemetry/LocationMessagePolicy.h/.cpp`
- Create: `tests/native/test_location_message_policy.cpp/.py`
- Modify later: `lib/tdeck_ui/UI/LXMF/UIManager.cpp/.h`

**Behavior:**
- Classify fields before `MessageStore::save_message()`.
- Valid telemetry with empty title/content updates location state silently.
- Telemetry plus text remains a normal persisted chat message and also updates location state.
- Malformed telemetry does not mutate location state; define whether an otherwise empty malformed frame is dropped and logged.
- Valid cease removes only the sender's location.
- Field source hash comes from the authenticated LXMessage envelope, never peer payload.
- All filesystem/network actions remain outside `LVGL_LOCK`.

**Tests:** pure policy matrix first, then a host harness with fake store/router/notification sinks proving exact side effects and call ordering.

**Commit:** `feat: classify inbound location messages`.

### Task 10: Integrate outbound LXMF telemetry without UI

**Objective:** Send initial/update/cease frames through the existing router with honest queue and persistence semantics.

**Files:**
- Create narrow adapter files if necessary under `lib/tdeck_ui/Telemetry/`.
- Modify: `lib/tdeck_ui/UI/LXMF/UIManager.cpp/.h`
- Add: host integration harness under `tests/native/`.

**Behavior:**
- Construct empty-content LXMF messages carrying field `0x02` and optional `0xFD`.
- Prefer opportunistic delivery; allow router promotion when required, matching short UI messages.
- Do not fake a known destination identity.
- Advance scheduler only after `handle_outbound` accepts ownership/queueing under the available API.
- Define persistence intentionally: transient periodic telemetry should not pollute conversation history; active consent/session state remains durable.
- Add test hooks only under `PYXIS_TEST_HOOKS` and audit them out of release builds.

**Tests:** exact fields, destination hash, method, empty content, failure/retry, cease shape, no chat-store write, no notification, no lock-held filesystem call.

**Commit:** `feat: route location telemetry over LXMF`.

### Task 11: Add SD tile-store core

**Objective:** Serve existing map tiles offline with bounded storage and no network dependency.

**Files:**
- Create: `lib/tdeck_ui/Hardware/TDeck/MapTileStore.h/.cpp`
- Create: portable index/filename helpers if needed.
- Create: `tests/native/test_map_tile_store.cpp/.py`

**Behavior:**
- Strict z/x/y validation and canonical relative paths.
- No path traversal.
- Configurable byte/file quotas; deterministic LRU or age eviction.
- Atomic tile writes and validation before visibility.
- Cache miss is normal, not an error.
- SD absence leaves map usable with placeholders and no effect on LittleFS.
- Tile index cannot grow without bound.

**Tests:** fake filesystem, hit/miss, corruption, interrupted write, quota exact boundaries, eviction, traversal attempts, duplicate tile, SD removal, 100k lookup stress.

**Commit:** `feat: add bounded offline map tile store`.

### Task 12: Add LVGL map screen with fixed reusable objects

**Objective:** Render and navigate the map without per-update object churn or blocking I/O under the LVGL lock.

**Files:**
- Create: `lib/tdeck_ui/UI/LXMF/MapScreen.h/.cpp`
- Modify: `ConversationListScreen.cpp/.h`, `UIManager.cpp/.h`, `library.json`.
- Add contract tests under `tests/build_scripts/` and pure presenter tests under `tests/native/`.

**Rules:**
- Fixed tile image object pool sized from tested viewport coverage.
- Fixed marker/label pool; update/hide objects in place.
- No SD read, tile decode, HTTP/TLS, LXMF routing, or persistence while holding `LVGL_LOCK`.
- Worker produces immutable completion records; loop/UI owner consumes them after generation check.
- Screen show/hide increments generation and cancels stale completions.
- Bounded work per `UIManager::update()` tick.
- Placeholders render immediately; map remains responsive without SD/Wi-Fi/GPS.

**Tests:** presenter state transitions on x86, stale-generation rejection, marker pool reuse, bounded queue overflow policy, source contracts preventing blocking calls under LVGL lock. Firmware build follows.

**Commit:** `feat: add bounded offline map screen`.

### Task 13: Add user-facing sharing controls

**Objective:** Expose clear, peer-specific, opt-in privacy controls after the transport core is proven.

**Files:** modify `ChatScreen.cpp/.h`, `SettingsScreen.cpp/.h` if global defaults are needed, and `UIManager` integration.

**Behavior:**
- Share button shows current peer and duration.
- Explicit start confirmation; default off.
- Stop sends cease and updates state only under defined queue policy.
- Visible active/expired/error state.
- Approximate-location radius is explicit if supported; otherwise do not expose it.
- Indefinite sharing has extra confirmation.
- No background sharing without a durable active session.

**Tests:** pure command mapping and state presenter x86 tests; source contract for every duration/button mapping; physical input test later.

**Commit:** `feat: add location sharing controls`.

### Task 14: Add optional bounded tile downloading last

**Objective:** Fetch missing tiles without recreating the prototype's TLS/heap/UI stalls.

**Prerequisite:** offline map and all host/firmware tests green.

**Rules:**
- Feature works without downloader.
- One long-lived/shared bounded worker or main-loop state machine; do not create/delete a 16 KiB task on every map open.
- Stream response to a temporary SD file in small fixed chunks; never allocate the full tile.
- Hard response-size limit, content-type/status checks, timeout, cancellation, and rate limiting.
- No process-wide mbedTLS allocator hook.
- Deduplicate queued tiles and apply backpressure.
- Configurable public endpoint with no private hosts or secrets in release image.
- Respect attribution/license requirements.

**Tests:** fake HTTP stream for partial reads, unknown length, oversized content, timeout, cancellation, redirects, non-200, malformed tile, dedupe, queue full, and stale-generation completion. ASan/UBSan and deterministic stress required.

**Commit:** `feat: download map tiles with bounded resources`.

### Task 15: Continuous verification and final acceptance

**Host gates after every production task:**

```bash
pytest tests/build_scripts tests/native -q
```

**Sanitizer gates:** run every new native executable with ASan/UBSan; no leaks, OOB, UAF, signed overflow, or timeout.

**Firmware gate:**

```bash
pio run -e tdeck
```

Record exact application SHA, microReticulum SHA, microLXMF SHA, PlatformIO environment, embedded version, firmware size, partition limits, and SHA-256.

**Release-safety inspection:** check final binary/diff for private endpoints, secrets, diagnostic flood, `PYXIS_TEST_HOOKS`, unsafe formatting, dependency pin drift, and filesystem writes.

**Physical acceptance matrix (pending until authorized and performed):**

1. Capture baseline free internal heap, largest block, PSRAM, LVGL task stack, loop stack, and uptime.
2. Open/close/pan/zoom map repeatedly with no SD, empty SD, warm cache, and cache misses.
3. Sustain map use while receiving announces/messages and while Wi-Fi/LoRa are active.
4. Verify no watchdog reset, lock timeout, UI starvation, or monotonic heap loss.
5. Pyxis → current Sideband and Columba location update.
6. Sideband and Columba → Pyxis location update.
7. Cease in both directions.
8. Expiry and resend cadence.
9. Mixed text+telemetry and malformed/adversarial field handling.
10. Reboot once and twice: same firmware/OTA slot; conversations, identity, settings, sessions, and received locations behave per policy.
11. Application-only update over a device with existing data; prove NVS/LittleFS preservation.

## 6. Stop conditions

Stop and redesign rather than patch around any of these:

- Production code cannot be compiled in x86 tests without broad Arduino/LVGL mocks.
- A peer field can cause unbounded allocation, nesting, cache growth, synchronous work, or blocking UI work.
- Filesystem I/O occurs while `LVGL_LOCK` is held.
- Telemetry periodic updates enter conversation history or trigger notifications.
- Session scheduler advances on attempted rather than accepted sends.
- Tile download requires a full-response allocation or global TLS allocator mutation.
- Firmware approaches an OTA slot or internal-heap safety threshold without measured headroom.
- A host test is treated as proof of physical behavior.

## 7. Immediate execution boundary

After review of this plan, begin only Tasks 0–4: native harness, pinned fixtures, telemetry decode/encode, and custom metadata. Do not touch LVGL, persistence, downloads, or physical hardware during the first implementation slice. The first deliverable is a portable codec backed by canonical vectors, adversarial tests, sanitizer results, the full existing host suite, and an exact `tdeck` compilation once PlatformIO is available.
