# LXST call-termination extension

Pyxis and compatible Columba builds use signalling status `0x07` (`TERMINATED`) as a backward-compatible active-call termination extension.

## Wire behavior

The frame uses the existing LXST signalling field:

```text
{ 0x00: [0x07] }
```

A normal local hangup sends three terminal frames with 20 ms bounded drain intervals, then performs the standard Reticulum `Link.teardown()`. The Link close remains the compatibility fallback.

## Receiver behavior

A compatible receiver accepts `TERMINATED` in every owned, non-idle call state and performs idempotent call cleanup. A later Link-close callback or duplicate terminal frame is harmless.

Columba intercepts `TERMINATED` before LXST-kt's legacy status handler. A link-scoped terminal gate suppresses duplicate terminal frames, while one atomic admission transaction and explicit call-generation tokens arbitrate outbound, inbound, terminal, Link-close, local-hangup, disable, and shutdown races. New calls are rejected until the prior Telephone state and Link ownership are both released. Every signalling and media callback must match the exact active Link and generation, so stale frames cannot mutate a replacement call. Exactly one path may invoke `Telephone.hangup()` in setup or active states. This keeps the extension outside the pinned LXST-kt dependency without translating it to `STATUS_AVAILABLE`.

## Compatibility

Current canonical Python LXST and older LXST-kt/Pyxis implementations do not define status `0x07`. Their existing decoders ignore unknown status values, after which the standard Link-close fallback remains available.

The terminal burst substantially reduces dependence on Reticulum's single unacknowledged `LINKCLOSE`, but it is not a remote acknowledgement. Pyxis therefore also expires an `ACTIVE` call after 90 seconds without valid inbound Codec2 media. A liveness refresh requires a non-empty, exactly frame-aligned Codec2 batch that the decoder accepts; truncated or trailing partial subframes do not count. LXST capture streams continuously even during silence, so this is a conservative orphan-call fallback rather than a short silence timer.

Physical acceptance must still verify both call directions, deliberate Link-close loss where practical, terminal loss, and the 90-second liveness path.
