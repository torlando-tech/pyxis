# Pyxis Functional-Parity Plan vs. Meshtastic Standalone UI / device-ui

## Goal
Bring `torlando-tech/pyxis` to **functional parity at the UX/product level** with the Meshtastic Standalone UI experience on T-Deck-class hardware, while **preserving Pyxis-native Reticulum strengths**:
- LXMF direct and propagated messaging
- LXST voice calling
- Reticulum transports (LoRa, AutoInterface, TCP, BLE)
- QR-based identity sharing

This is **not** a literal code port. The Meshtastic codebase is protocol-specific and centered on the Meshtastic Client API; Pyxis owns its own application stack around Reticulum/LXMF/LXST.

## Product Parity Definition
Treat parity as:
- First-boot setup wizard
- Home dashboard
- Peer/node directory with filtering and highlighting
- Channel-equivalent navigation surface
- Threaded chats with delivery states
- Offline map with peers + own location
- Settings + tools/diagnostics
- Lock/sleep/system actions
- Status bar / hardware indicators
- SD-card-centered offline workflows

Do **not** treat parity as:
- Meshtastic protobuf compatibility
- Meshtastic radio-region/channel-key semantics
- Booting into Meshtastic BaseUI
- Multi-device support in phase 1

## Interpretation of Meshtastic Concepts for Reticulum
Because Pyxis is Reticulum/LXMF-based, some MUI concepts must be adapted:
- **Nodes list** -> **Peers / announces / known routes / known contacts**
- **Channels** -> **Destinations / interfaces / conversation classes** (not literal PSK channels)
- **Trace route** -> **Route inspection / path lookup / hop/interface diagnostics**
- **Mesh detector** -> **Announce scanner / nearby-peer discovery**
- **Signal scanner** -> **Interface/link quality monitor**
- **Packet log** -> **Reticulum TX/RX + announce + propagation event log**

## Current State Summary
Pyxis already has:
- A T-Deck-only PlatformIO firmware target
- Existing LVGL UI stack with screens for conversations, chat, compose, announces, status, settings, QR, propagation nodes, and calls
- Reticulum/LXMF/LXST core already integrated in firmware
- Multiple transport interfaces: TCP, LoRa, AutoInterface, BLE
- Existing PR build workflow and firmware release/flasher workflow
- Existing Python interop tests around LXST/Codec2 wire format and pipeline behavior

Pyxis does **not yet expose** the following MUI-equivalent surfaces as first-class UI modules:
- First-boot setup wizard
- Dashboard/home screen
- Peer/node explorer with rich filters/highlights/details
- Offline map UI
- Tools/diagnostics suite
- Lock/sleep/system action experience on par with MUI
- Multi-screen app shell that feels like a cohesive handheld client rather than a messaging-first tool

## Recommended Delivery Strategy
1. **Refactor just enough first** to support rapid feature delivery.
2. Build parity in **small vertical slices**.
3. Keep all work **T-Deck first** until parity is reached.
4. Preserve existing messaging/call flows during refactor.
5. Reuse MUI ideas at the **product and architecture level**, not protocol level.

## Architecture Recommendation
### 1) Separate firmware bootstrap from app logic
Shrink `src/main.cpp` into a bootstrap/composition root only.

Create service-layer modules:
- `AppStateStore`
- `SettingsService`
- `PeerDirectoryService`
- `RouteDiagnosticsService`
- `MapService`
- `StatsService`
- `PacketLogService`
- `IdentityShareService`
- `SystemActionsService`

### 2) Add an event-driven UI model
Create a unidirectional flow:
- transport/router/interfaces emit events
- reducers/services update state snapshots
- screens render from snapshot state

Recommended primitives:
- `EventBus`
- `AppState` snapshot structs
- reducers / updaters
- polling adapters only where necessary for hardware APIs

### 3) Formalize UI shell + navigation
Create a reusable shell layer:
- `ScreenBase`
- `NavController`
- `StatusBar`
- `BottomNav`
- `ModalManager`
- `FormComponents`
- `ListCell` variants
- `MapCanvas`

### 4) Create persistence boundaries
Persist separately:
- NVS: settings, toggles, onboarding completion, lock preferences
- filesystem/SD: offline maps, packet logs, export files, future cached peer metadata

### 5) Standardize shared models
Create explicit models instead of screen-local ad hoc structs:
- `PeerSummary`
- `PeerDetails`
- `ConversationSummary`
- `InterfaceStatus`
- `RouteSummary`
- `MapMarker`
- `SystemHealth`
- `ToolStats`

## Target UI Information Architecture
### Primary surfaces
1. **Home**
2. **Peers**
3. **Messages**
4. **Map**
5. **Settings & Tools**

### Secondary screens
- Peer details
- Compose / new chat
- Propagation nodes
- Call screen
- Identity QR / import/export
- Packet log
- Route diagnostics
- Signal monitor
- Maintenance/system actions

## Delivery Phases

### Phase 0 — Parity Contract + Refactor Guardrails
**Objective:** define the product boundary and prevent architecture drift.

Deliverables:
- `docs/parity/README.md` with scope, mapping, non-goals
- screen inventory of current Pyxis vs target parity
- state/data-flow diagram
- branch/PR strategy

Acceptance:
- no code behavior change yet
- written architecture doc exists
- all contributors align on UX parity interpretation

### Phase 1 — App Shell Foundation
**Objective:** make future screens cheap to build.

Implement:
- `ScreenBase`
- `NavController`
- shared header/status bar/footer components
- central `AppStateStore`
- event bus adapters for:
  - conversations/messages
  - announces/known peers
  - interface statuses
  - battery/GPS/WiFi/BLE
  - propagation state

Acceptance:
- existing conversation/status/settings/announces screens run through new shell without regression
- both `tdeck` and `tdeck-bluedroid` still compile

### Phase 2 — First-Boot Setup Wizard
**Objective:** match MUI’s “device usable without external client” feeling.

Implement wizard steps:
- display name / identity label
- WiFi credentials
- transport enablement defaults
- LoRa interface defaults if enabled
- announce/privacy defaults
- optional GPS time sync default
- storage/SD check

Behavior:
- show only on first boot or after factory reset
- save once, avoid multiple reboot cycles
- deep-link to advanced settings later

Acceptance:
- onboarding completion flag persisted
- clean recovery path if WiFi or SD init fails

### Phase 3 — Home Dashboard
**Objective:** create the MUI-equivalent landing surface.

Dashboard cards/widgets:
- unread conversations
- known peers count
- routes / reachable peers
- current interfaces summary (TCP, LoRa, Auto, BLE)
- selected propagation node / sync health
- GPS state / position lock / satellite count
- battery / uptime / storage
- current time

Actions:
- tap to open peers/messages/map/settings
- long-press for quick actions where appropriate

Acceptance:
- dashboard becomes default post-onboarding entry point
- state refresh feels live but does not fragment heap or block UI

### Phase 4 — Peer Directory (Nodes List Equivalent)
**Objective:** turn the existing announce list into a real node browser.

Implement:
- merged data source from announces + conversations + known paths + propagation nodes
- peer list rows showing:
  - name / truncated hash
  - availability / last heard
  - hops
  - best interface
  - propagation capability
  - unread / recent activity markers
  - optional location badge
- detail screen with:
  - identity / address
  - last heard
  - route summary
  - interface seen on
  - BLE RSSI or WiFi RSSI if relevant
  - location/time if shared
  - direct message / call / select propagation / copy hash / show QR

Acceptance:
- users can discover peers without already having a conversation
- current announce screen can be retired or downgraded to a filtered subview

### Phase 5 — Filters, Highlights, and Search
**Objective:** reach MUI’s node-list usability.

Implement filter dimensions:
- online/recent/offline
- interface type
- hop count range
- has path / no path
- propagation capable
- has location
- has unread
- direct-only vs propagated-capable

Implement highlight modes:
- best RSSI / nearest hop count
- recently heard
- unread
- located peers
- favorites / pinned peers

Acceptance:
- filters are fast on-device
- state survives navigation; optional persistence is fine

### Phase 6 — Channel-Equivalent Surface
**Objective:** solve the “Channels” parity gap honestly.

Create a new **Networks** or **Destinations** screen with tabs:
- **Interfaces** — TCP / LoRa / Auto / BLE state and controls
- **Peers** — direct destinations
- **Propagation** — propagation nodes / strategy
- **Saved destinations/groups** — future-compatible bucket

Do **not** fake Meshtastic channels. Instead, expose the real Reticulum/LXMF concepts clearly.

Optional future extension:
- support named group destinations or broadcast-style abstractions if microReticulum capabilities allow it

Acceptance:
- users understand how traffic leaves the device
- screen serves the same navigational role MUI channels serve

### Phase 7 — Chat Parity Hardening
**Objective:** polish the messaging UX to parity level.

Keep existing conversation/chat/compose stack, but add:
- richer delivery states (queued, path resolving, delivered, propagation fallback, failed)
- contact cards in header
- faster jump from peer list -> compose -> chat
- unread badges across shell
- message action sheet (copy, delete local, retry)
- better empty states
- clearer propagation indicators for delayed/offline delivery

Acceptance:
- no regression in existing messaging or calling
- chat status is understandable without logs

### Phase 8 — Offline Map (High-Value Gap)
**Objective:** add one of MUI’s biggest differentiators.

Implement:
- SD-backed tile loader
- MUI-compatible tile layout if possible:
  - `/map`
  - `/maps/{STYLE}/`
- pan / zoom
- own-position centering
- home recentering
- map style selection
- brightness/contrast options
- peer markers
- marker details / quick-open peer

Needed protocol/data work:
- define or extend announce payload/app data to optionally carry position
- add privacy toggle for location sharing
- cache peer positions separately from transient route state

Performance constraints:
- tile cache must be bounded
- SD I/O must not stall UI thread
- prefer background decode/preload where possible

Acceptance:
- map remains responsive on T-Deck memory budget
- works with no network once tiles exist on SD

### Phase 9 — Tools & Diagnostics Suite
**Objective:** build the MUI “Settings & Tools” parity layer.

Tools screen should include:
- **Announce Scanner** (MUI Mesh Detector equivalent)
- **Signal Monitor** (MUI Signal Scanner equivalent)
- **Route Inspector** (rnpath-style path/hop/interface summary)
- **Statistics** (messages, announces, calls, propagation sync, transport counters)
- **Packet Log** (TX/RX events and important state transitions)
- **Storage / export** (optional text export to SD)

Important note:
- a literal Meshtastic-style traceroute may not be possible; if per-hop detail is unavailable, ship route inspection first

Acceptance:
- tools are useful without serial logs
- diagnostics are human-readable on-device

### Phase 10 — Settings Redesign
**Objective:** split configuration from status and reach MUI’s structure.

Recommended tabs/sections:
- Identity
- Network interfaces
- Messaging & propagation
- Display & input
- Sound & notifications
- GPS & time
- Storage & maps
- Privacy & sharing
- Maintenance

Move current status-only data out of Settings into:
- Home dashboard
- Status bar
- dedicated status/detail screens

Acceptance:
- settings screen is no longer overloaded
- save/reconnect flows are predictable

### Phase 11 — System Actions / Lock / Sleep / Maintenance
**Objective:** match MUI’s handheld behavior.

Implement:
- lock screen
- sleep action
- reboot action
- factory reset
- quiet/radio-silent mode
- maintenance mode (serial/flasher/OTA-oriented) as Pyxis equivalent to MUI’s programming/base-mode affordances

Acceptance:
- long-press actions are consistent
- accidental destructive actions require confirmation

### Phase 12 — Polish, Localization, and Future Portability
**Objective:** finish the experience and prepare for broader hardware later.

Add:
- themes / visual polish
- localization framework
- text truncation / font fallback review
- performance/heap tuning
- optional future host simulator / SDL test target
- hardware abstraction cleanup for eventual non-T-Deck ports

Acceptance:
- UI feels cohesive, not bolted-on
- memory and uptime are stable for multi-day use

## Suggested Repo Layout Changes
Add new folders:
- `lib/pyxis_app_core/`
- `lib/pyxis_services/`
- `lib/tdeck_ui/UI/Shell/`
- `lib/tdeck_ui/UI/Shared/`
- `lib/tdeck_ui/UI/Home/`
- `lib/tdeck_ui/UI/Peers/`
- `lib/tdeck_ui/UI/Map/`
- `lib/tdeck_ui/UI/Tools/`
- `docs/parity/`

Keep existing:
- `lib/tdeck_ui/UI/LXMF/` for messaging/call-specific screens

Recommended file moves:
- move non-UI orchestration out of `src/main.cpp`
- create `src/bootstrap/` or `lib/pyxis_runtime/`

## PR-by-PR Backlog (Recommended)
### PR 1 — Architecture skeleton
- add docs/parity
- add AppState, EventBus, ScreenBase, NavController
- wire existing screens through shell

### PR 2 — Service extraction from `main.cpp`
- create transport supervisor
- create state update loop
- move persistence concerns out of UI code

### PR 3 — Setup wizard
- first-boot flow
- schema migration for settings

### PR 4 — Dashboard v1
- basic cards + quick actions

### PR 5 — Peer directory core
- merge announce/conversation/route data
- peer detail screen

### PR 6 — Filters/highlights/search
- persistent filter state
- favorites/pins

### PR 7 — Networks/Destinations surface
- interface health + controls
- route overview

### PR 8 — Messaging parity polish
- delivery state badges
- retry/delete/action sheet
- unread integration across shell

### PR 9 — Offline map core
- tile loader, marker model, map canvas

### PR 10 — Map polish + location sharing
- style selection
- privacy toggle
- recenter/home controls

### PR 11 — Tools suite
- announce scanner
- signal monitor
- route inspector
- statistics
- packet log

### PR 12 — Settings redesign
- tabs/sections
- maintenance actions

### PR 13 — Lock/sleep/maintenance mode
- lock UI
- sleep/reboot/reset

### PR 14 — CI/test hardening
- run Python interop tests in CI
- add unit tests for state/services
- add docs + screenshots

### PR 15 — Polish + localization scaffold
- strings extraction
- theming cleanup
- memory tuning

## Testing Strategy
### Must-pass on every PR
- `pio run -e tdeck`
- `pio run -e tdeck-bluedroid`
- Python interop test suite under `tests/interop`

### Add new tests for parity work
- settings migration tests
- peer directory reducer tests
- route summary tests
- map tile coordinate math tests
- packet log ring buffer tests
- dashboard snapshot rendering tests (model-level)

### Hardware validation checklist
- 24h idle soak
- 2h mixed UI navigation soak
- message send/receive direct
- message send/receive via propagation
- incoming/outgoing call sanity
- WiFi reconnect stress
- BLE enable/disable stress
- SD card insert/remove map refresh
- low battery + resume behavior

## Definition of Done for “Parity Beta”
Pyxis reaches parity beta when a T-Deck user can, from the device alone:
- complete first-time setup
- browse peers and inspect details
- read/send messages comfortably
- see delivery/route state clearly
- select and inspect propagation nodes
- use an offline map with peer markers
- access diagnostics without serial logs
- manage display/network/privacy settings
- lock/sleep/reboot from UI

while preserving:
- voice calls
- QR identity sharing
- existing transports
- flasher/release flows

## Explicit Non-Goals for the First Parity Push
- porting MUI code directly
- Meshtastic protocol compatibility
- multi-device support before T-Deck parity
- perfect one-to-one cloning of Meshtastic terminology
- advanced remote management beyond what Reticulum APIs safely expose

## Risks and Mitigations
### Risk: giant rewrite stalls progress
Mitigation:
- force PR-sized slices
- keep old screens alive until replacements are ready

### Risk: map feature blows memory budget
Mitigation:
- bounded tile cache
- staged implementation
- T-Deck profiling before polish

### Risk: route diagnostics exceed microReticulum capability
Mitigation:
- ship route summary first
- treat per-hop trace as optional enhancement

### Risk: UI thread blocking from storage/network work
Mitigation:
- event queue + background-ish service loops
- no SD tile decode on synchronous input path

### Risk: feature creep into desktop/multi-device support
Mitigation:
- T-Deck-only parity milestone
- portability after parity beta

## Working Instructions for Codex / Claude
Use this as the execution contract:

1. Do not port Meshtastic protocol code.
2. Use Meshtastic MUI as a UX and architecture reference only.
3. Preserve existing Pyxis messaging, propagation, QR sharing, and voice-call behavior.
4. Work in PR-sized increments; no mega-commits.
5. Keep `src/main.cpp` shrinking over time, not growing.
6. Build both `tdeck` and `tdeck-bluedroid` before concluding each task.
7. Run Python interop tests whenever touching call/audio/wire code.
8. Prefer additive refactors with compatibility shims over destructive rewrites.
9. T-Deck first. No multi-device abstractions unless they reduce code, not add complexity.
10. Every new screen must be wired through shared shell/navigation primitives.
11. Every user-visible feature must have a state model, not screen-local hidden logic.
12. Update `docs/parity/` after each major milestone.

## Starter Prompt for Codex / Claude
You are implementing MUI-inspired functional parity in `torlando-tech/pyxis` for T-Deck hardware. This is a Reticulum/LXMF/LXST project, not a Meshtastic project. Do not port Meshtastic protocol code. Use Meshtastic MUI as a product and UI architecture reference only.

Constraints:
- Preserve existing messaging, propagation, QR sharing, and voice-call flows.
- Work in small, reviewable PR-sized steps.
- Build both PlatformIO environments: `tdeck` and `tdeck-bluedroid`.
- When touching voice/call/audio code, also run the Python interop tests in `tests/interop`.
- Shrink `src/main.cpp` over time by extracting services and app state.
- T-Deck first; do not generalize for other hardware unless it makes the code simpler.

Current milestone:
[PASTE CURRENT PR/PHASE HERE]

Required output:
1. brief design note
2. file-by-file plan
3. implementation
4. build/test results
5. follow-up risks

## Best First Task
Start with **PR 1 — Architecture skeleton**:
- add `docs/parity/README.md`
- add `ScreenBase`, `NavController`, `StatusBar`, `AppStateStore`, and `EventBus`
- route existing screens through the new shell without changing product behavior
- keep builds green
