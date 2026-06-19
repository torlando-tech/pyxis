#!/usr/bin/env bash
#
# Full end-to-end on-device test for pyxis: build + flash a T-Deck, bring up a
# Mac-side LXMF echo bot over the local rnsd, and drive LXMF round-trips
# (DIRECT / OPPORTUNISTIC / optional PROPAGATED) plus a bz2-on-receive probe
# via the firmware's `T:` serial command surface (-DPYXIS_TEST_HOOKS).
#
# All environment-specific values are read from the environment (nothing
# deployment-specific is committed):
#   PYXIS_TEST_TCP_HOST   Mac IP the T-Deck dials for rnsd (default: en0 IPv4)
#   PYXIS_TEST_TCP_PORT   rnsd TCPServerInterface port      (default: 4242)
#   PYXIS_SERIAL_PORT     T-Deck USB serial                 (default: first /dev/cu.usbmodem*)
#   PYXIS_ENV             platformio env                    (default: tdeck)
#   PYXIS_PROPAGATION_NODE_HEX  lxmd PN hash (optional; enables PROPAGATED rounds)
#
# Usage:
#   tests/hardware/run_e2e.sh                 # build, flash, one pass + bz2 probe
#   tests/hardware/run_e2e.sh --soak-hours 1  # ... then soak round-trips for 1h
#   PYXIS_SKIP_FLASH=1 tests/hardware/run_e2e.sh   # reuse already-flashed fw
#
# Exits 0 only if every round (incl. the bz2 probe) passed and no crash fired.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SOAK_HOURS="0"
[ "${1:-}" = "--soak-hours" ] && SOAK_HOURS="${2:-0}"

PYXIS_ENV="${PYXIS_ENV:-tdeck}"
PYXIS_TEST_TCP_PORT="${PYXIS_TEST_TCP_PORT:-4242}"
PYXIS_TEST_TCP_HOST="${PYXIS_TEST_TCP_HOST:-$(ipconfig getifaddr en0 2>/dev/null || true)}"
PYXIS_SERIAL_PORT="${PYXIS_SERIAL_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
PIO="$(command -v pio || echo /opt/homebrew/bin/pio)"
# The harness needs pyserial; PlatformIO's bundled python has it. Prefer an
# explicit PYXIS_HARNESS_PY, else the penv python, else any python with serial.
PIO_PY="${PYXIS_HARNESS_PY:-}"
[ -z "$PIO_PY" ] && PIO_PY="$(ls "$HOME"/.platformio/penv/bin/python3 2>/dev/null | head -1 || true)"
[ -z "$PIO_PY" ] && for c in /opt/homebrew/Cellar/platformio/*/libexec/bin/python3; do [ -x "$c" ] && PIO_PY="$c" && break; done

echo "== pyxis on-device e2e =="
echo "  repo:        $REPO"
echo "  env:         $PYXIS_ENV"
echo "  serial:      ${PYXIS_SERIAL_PORT:-<none found>}"
echo "  rnsd target: ${PYXIS_TEST_TCP_HOST:-<unset>}:$PYXIS_TEST_TCP_PORT"
echo "  prop node:   ${PYXIS_PROPAGATION_NODE_HEX:-<unset — PROPAGATED skipped>}"
echo "  soak hours:  $SOAK_HOURS"
echo "  harness py:  ${PIO_PY:-<not found>}"

[ -z "${PYXIS_SERIAL_PORT:-}" ] && { echo "ERROR: no T-Deck serial port found (set PYXIS_SERIAL_PORT)"; exit 2; }
[ -z "${PYXIS_TEST_TCP_HOST:-}" ] && { echo "ERROR: could not determine Mac IP (set PYXIS_TEST_TCP_HOST)"; exit 2; }

# rnsd must be listening so both the T-Deck (LAN) and the bot (127.0.0.1) can
# attach to the same Reticulum hub.
if ! lsof -nP -iTCP:"$PYXIS_TEST_TCP_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "ERROR: nothing listening on TCP:$PYXIS_TEST_TCP_PORT — start rnsd with a"
    echo "       TCPServerInterface on that port first (e.g. \`rnsd\`)."
    exit 2
fi

export PYXIS_TEST_TCP_HOST PYXIS_TEST_TCP_PORT PYXIS_SERIAL_PORT PYXIS_PROPAGATION_NODE_HEX

if [ "${PYXIS_SKIP_FLASH:-0}" != "1" ]; then
    echo "== build + flash ($PYXIS_ENV) with TCP target baked in =="
    "$PIO" run -e "$PYXIS_ENV" -t upload --upload-port "$PYXIS_SERIAL_PORT"
fi

echo "== running harness (resets device, drives T: commands) =="
exec "$PIO_PY" "$HERE/tdeck_harness.py" --soak-hours "$SOAK_HOURS"
