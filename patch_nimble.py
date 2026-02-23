"""
PlatformIO pre-build script: Patch NimBLE ble_hs.c assert(0) in timer expiry handler.

NimBLE's ble_hs_timer_exp() asserts when a timer fires during BLE_HS_SYNC_STATE_BRINGUP.
This is a race condition (timer scheduled before host reset wasn't cancelled), not a fatal
error. The assert kills the ESP32, corrupting any file writes in progress.

Fix: Replace assert(0) with break — the timer is harmless during bringup; the sync
process will reschedule timers when transitioning to GOOD state.
"""
Import("env")
import os

def patch_nimble_ble_hs(env):
    ble_hs_path = os.path.join(
        env.get("PROJECT_DIR", "."),
        ".pio", "libdeps", "tdeck",
        "NimBLE-Arduino", "src", "nimble", "nimble", "host", "src", "ble_hs.c"
    )

    if not os.path.exists(ble_hs_path):
        print("PATCH: NimBLE ble_hs.c not found, skipping patch")
        return

    with open(ble_hs_path, "r") as f:
        content = f.read()

    # Only patch if the assert is still there (idempotent)
    old = """    case BLE_HS_SYNC_STATE_BRINGUP:
    default:
        /* The timer should not be set in this state. */
        assert(0);
        break;"""

    new = """    case BLE_HS_SYNC_STATE_BRINGUP:
    default:
        /* Timer can fire during bringup due to race with host reset.
         * This is harmless — bringup will reschedule when ready. */
        break;"""

    if old in content:
        content = content.replace(old, new)
        with open(ble_hs_path, "w") as f:
            f.write(content)
        print("PATCH: Patched NimBLE ble_hs.c -- removed assert(0) in BRINGUP timer handler")
    elif new in content:
        print("PATCH: NimBLE ble_hs.c already patched")
    else:
        print("PATCH: WARNING -- Could not find expected code in ble_hs.c, manual review needed")

# Run the patch immediately during script evaluation (before any build targets)
patch_nimble_ble_hs(env)
