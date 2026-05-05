# Pyxis User Manual

## What Pyxis Is

Pyxis is a handheld LXMF and LXST client for the LilyGO T-Deck Plus. It lets you:

- exchange LXMF messages
- browse the announce stream and open chats from discovered peers
- sync through propagation nodes
- place and receive LXST voice calls
- view GPS position and peer telemetry on a map
- use multiple Reticulum transports, including TCP, LoRa, Wi-Fi auto discovery, and BLE

The project is still marked as work in progress. Some features are stable enough for daily use, while others are experimental.

## Supported Hardware

Pyxis is built for:

- *LilyGO T-Deck* (Plus for GPS)
- ESP32-S3 with 8 MB flash
- built-in keyboard, trackball, touchscreen, GPS, speaker, and microphone

Recommended extras:

- a USB-C data cable
- a microSD card if you want offline map tile storage
- Wi-Fi access if you want TCP transport, Auto Discovery, or map tile downloads
- a compatible LoRa/RNode setup if you want radio transport

## Installing or Updating Firmware

The repository includes a web flasher at [`docs/flasher/index.html`](flasher/index.html).

### Requirements

- Chrome, Edge, or Opera
- USB-C data cable
- T-Deck Plus connected by USB

### Install Steps

1. Open the flasher page.
2. Connect the T-Deck Plus by USB-C.
3. Click `Flash Firmware`.
4. Select the serial port for the device when the browser prompts you.
5. Wait for flashing to finish and the device to reboot.

### Full Install vs Update

- Normal update: writes only the main firmware and keeps existing settings and messages.
- Full install: writes bootloader, partitions, and firmware, and erases stored settings and message data.

Use a full install for the first flash or when you want a clean reset.

## First Boot

On boot, Pyxis:

- loads saved settings
- initializes GPS
- tries to sync time from GPS if enabled
- connects to Wi-Fi if credentials are saved
- starts enabled interfaces such as TCP, LoRa, Auto Discovery, and BLE
- loads the LXMF message store

If GPS time is not available immediately, Pyxis falls back to network time after Wi-Fi connects.

## Basic Controls

You can use Pyxis with touch, the trackball, and the physical keyboard.

- Touchscreen: tap buttons, lists, and dialogs.
- Trackball: move focus between UI elements.
- Trackball press: activate the currently focused item.
- Keyboard: type in text fields and compose messages.

Useful behaviors from the (0.2) UI:

- Long-press a conversation to delete it.
- Long-press a chat message to copy it.
- Long-press the chat input field to paste from the internal clipboard.

## Main Screens

### Conversations

This is the default home screen.

It shows:

- your conversation list
- unread counts
- the last message preview
- a top status strip for Wi-Fi, LoRa, GPS, BLE, and battery
- a refresh button for propagation sync
- bottom navigation buttons for compose, announces, status, map, and settings

Top status indicators:

- Wi-Fi: signal strength or `--` when disconnected
- LoRa: last radio RSSI when available
- GPS: satellite count when a fix is available
- BLE: central and peripheral connection counts
- Battery: charge icon when externally powered, percentage when on battery

### New Message

Open the compose screen from the envelope button on the bottom navigation bar.

To send a new message:

1. Enter the destination hash.
2. Enter the message body.
3. Press `Send`.

The destination must be a 32-character hexadecimal LXMF address.

### Chat

Selecting a conversation opens the chat screen.

Available actions:

- `Back`: return to the conversation list
- `GPS` button: start or stop location sharing with that peer
- `Call` button: start an LXST voice call
- `Send`: send the current text message

Delivery indicators for outgoing messages:

- single check: sent
- double check: delivered
- `X`: failed

Location sharing options currently available:

- 15 minutes
- 1 hour
- 4 hours
- until midnight
- indefinite
- stop sharing

### Announces

The announce screen shows recently discovered LXMF destinations.

You can:

- tap an entry to open a chat with that destination
- refresh the list
- send your own announce

This is the easiest way to start messaging a peer you have discovered but have not messaged before.

### Status

The status screen is the main diagnostics view. It shows:

- uptime
- device identity hash
- LXMF delivery address
- Wi-Fi connection state, IP, and RSSI
- LoRa connection status
- Reticulum connection state
- currently selected propagation node
- BLE peer details when BLE is enabled

Press `Share` on this screen to show a QR code for identity sharing.

### QR Share

The QR screen displays your identity and LXMF address as a scannable code so another device (e.g. Columba) can add you more easily.

### Map

The map screen shows:

- your GPS position
- peer telemetry markers
- offline or cached map tiles stored on the SD card

Map controls:

- `+` and `-` buttons: zoom
- touch-drag: pan
- keyboard arrow keys: pan
- keyboard `+` or `-`: zoom
- keyboard `C`: re-center on your GPS position

Important behavior:

- Missing tiles are downloaded from OpenStreetMap when Wi-Fi is connected.
- Downloaded tiles are written to the SD card.
- If there is no SD card or no Wi-Fi, the map can only show tiles that are already available locally.
- Manual panning disables follow-GPS mode until you re-center.

#### First-Time Map Setup

For the map to work as expected on a new device:

1. Insert a microSD card before opening the map.
2. Configure Wi-Fi in `Settings`, save, and reconnect if needed.
3. Wait for a GPS fix so Pyxis knows where to center the map.
4. Open the map screen and let it download the visible tiles for your current area.

Pyxis stores tiles at `S:tiles/{z}/{x}/{y}.png` on the SD card. The map does not preload a world basemap. It only downloads the tiles for the area and zoom levels you actually view.

#### What You Need To See Maps

The map view depends on these conditions:

- Wi-Fi connected for first-time tile downloads
- SD card inserted and mounted for tile storage
- GPS fix available if you want the map centered on your position

If one of those conditions is missing, the map may open but stay blank except for the background color.

#### Peer Markers

Peer markers only appear after Pyxis receives telemetry from another device. Regular text messages do not create map markers by themselves.

To test peer markers:

1. Open a chat with the peer.
2. Start location sharing from the `GPS` button in chat.
3. Wait for the peer to send telemetry.
4. Open the map screen again if needed.

### Settings

The settings screen saves directly to device storage and most changes apply immediately after `Save`.

Sections currently exposed in the UI:

#### Network

- Wi-Fi SSID
- Wi-Fi password
- TCP host
- TCP port
- `Reconnect` button

Default TCP server:

- `sideband.connect.reticulum.network`
- port `4965`

#### Identity

- display name broadcast in announces and shown to peers when available

#### Display

- brightness slider
- keyboard backlight toggle
- screen timeout: `30 sec`, `1 min`, `5 min`, or `Never`

Brightness changes apply immediately, even before you save.

#### Notifications

- message notification sound on or off
- notification volume from `0` to `100`

#### Interfaces

- Auto Discovery
- BLE P2P
- TCP Interface
- LoRa Interface

LoRa settings appear when LoRa is enabled:

- frequency in MHz
- bandwidth: `62.5`, `125`, `250`, or `500 kHz`
- spreading factor: `7` through `12`
- coding rate: `4/5` through `4/8`
- TX power

#### Delivery

- propagation nodes view
- `Fallback to Prop`
- `Propagation Only`

Use the propagation nodes view to:

- keep auto-select enabled
- manually select a discovered node
- enter a 32-character node hash manually
- request an immediate propagation sync

#### GPS Status

- satellite count
- current coordinates
- altitude
- HDOP quality

#### System Info

- identity
- LXMF address
- firmware version
- free storage
- free RAM

#### Advanced

- announce interval in seconds
- propagation sync interval in minutes
- GPS time sync on or off

Setting an interval to `0` disables that periodic activity.

## Voice Calls

Pyxis supports LXST voice calls (Initial implementation) from the chat screen.

Call states visible in the UI:

- connecting
- ringing
- incoming call
- in call
- ended

During a call you can:

- mute or unmute yourself
- end the call

For incoming calls:

- the left button becomes `Answer`
- the right button becomes `Reject`

Current implementation notes from the codebase:

- calls use a low-bandwidth Codec2 profile
- call setup depends on Reticulum path discovery and link establishment
- audio quality is currently described in the project as rough

## Data Persistence

Pyxis stores:

- settings in NVS
- messages and identity data in flash storage
- map tiles on the SD card when available

Normal firmware updates preserve stored settings and messages. A full install wipes them.

## Troubleshooting

### No timestamps or messages show as "Future"

The device clock is not synced yet. Wait for a GPS fix or connect Wi-Fi so time can be synced over the network.

### Wi-Fi features do not work

Check the following in Settings:

- SSID and password
- TCP Interface enabled if you need TCP transport
- Auto Discovery enabled only when Wi-Fi is connected

Without Wi-Fi, these features will not work:

- TCP transport
- Auto Discovery
- downloading new map tiles
- propagation sync through network nodes

### LoRa does not come up

Verify:

- LoRa Interface is enabled
- frequency and bandwidth match others on your network
- spreading factor, coding rate, and TX power are valid for your deployment

### BLE is unstable

BLE is still treated as experimental in the project. If the device is unstable, disable BLE first.

### Voice calls fail to connect

Check:

- you can already exchange messages with the peer
- a path exists to the peer
- the peer also supports LXST

If voice is unreliable, messaging is the safer fallback.

### Map is empty

Check:

- Wi-Fi is configured and connected
- an SD card is inserted and mounted
- GPS has a fix if you expect the map to center on your location
- you have opened the map while Wi-Fi was available so tiles could be downloaded
- the peer has actually sent telemetry if you expect peer markers

## Known Limitations

The current code and README indicate these rough edges:

- BLE support is incomplete
- voice calling works but is still rough
- general stability is still a work in progress

Treat Pyxis as an actively developing field client rather than a finished consumer product.
