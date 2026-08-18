<p align="center">
  <img src="pyxis-icon.svg" width="200" alt="Pyxis logo">
</p>

# Pyxis
An LXMF and LXST client firmware for T-Deck, built on a [highly modified fork](https://github.com/torlando-tech/microReticulum/tree/feat/t-deck) of [microReticulum](https://github.com/attermann/microReticulum)

Very much WIP, don't expect stability :) 

## Features
Reticulum transport over: 
- RNode-compatible LoRa
- AutoInterface (local wifi)
- TCP Client
- BLE Interface (barely working if at all)

Other features:
- GPS-synced time
- One really annoying beep when you get a new message (user toggle)
- View the announce stream
- Choose a propagation node (and sync with it) (fixed in v0.2.1)
- Set auto announce timer
- Light up keyboard (user toggle)
- ~~Will crash in about 5 minutes of normal use (sorry)~~ I had 5d uptime on v0.2.0 with BLE disabled
- Make LXST voice calls (codec2 only, quality sounds horrible coming out the other end in Columba, needs work)

## Flashing

The easiest way to get Pyxis running is the [web flasher](https://torlando-tech.github.io/pyxis/flasher/), which downloads release firmware from this repository's releases and verifies each image's SHA-256 digest against the release metadata.

For esptool (or any other tool that writes raw flash), each release also publishes a **merged binary** (`pyxis-<tag>-merged.bin`) that contains the bootloader, partition table, OTA selector, and application at their fixed offsets. Provision a T-Deck Plus (8 MB flash) with:

```
esptool.py --chip esp32s3 erase_flash
esptool.py --chip esp32s3 write_flash 0x0 pyxis-<tag>-merged.bin
```

**Merged binaries are for first install / provisioning.** Flashing one overwrites every flash region, including NVS (settings, identity) and the LittleFS partition (messages, paths, maps). To update an existing device without losing data, flash `firmware.bin` to `0x10000` only, or use the Columba-compatible `pyxis-<tag>.pyxis.zip` update package.

## Why "Pyxis"
Pyxis, latin for "compass," is a [constellation](https://en.wikipedia.org/wiki/Pyxis) in the southern sky depicting a mariner's compass. Small but essential, the compass ensures every message finds its destination - even when the path is uncertain. 
