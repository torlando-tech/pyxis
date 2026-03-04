"""
PlatformIO pre-build script: Patch NimBLE stability issues.

Patch 1 — ble_hs.c: Remove assert(0) in BLE_HS_SYNC_STATE_BRINGUP timer handler.
  Timer can fire during host re-sync due to a race condition. Harmless — just ignore it.

Patch 2 — NimBLEClient.cpp: Add null checks in PHY update event handler.
  If a client is deleted while events are queued, the callback arg becomes a dangling
  pointer. Guard against null pClient and null m_pClientCallbacks.

Patch 3 — ble_gap.c: Handle BLE_ERR_CONN_ESTABLISHMENT (574) in conn complete handler.
  NimBLE only handles 574 when BLE_PERIODIC_ADV_WITH_RESPONSES is enabled. Without this
  patch, 574 falls through to assert(0) and the master GAP state is never cleaned up,
  leaving scan/advertise permanently broken. The ESP32-S3 controller returns 574 when
  connection establishment fails (peer disappeared, RF interference, etc.).

Patch 4 — NimBLEDevice.cpp: Expose host reset reason via global volatile variable.
  NimBLE's onReset callback logs reason to ESP_LOG (serial only). This patch adds a
  global volatile int that application code can poll to capture the reason in UDP logs.
"""
Import("env")
import os

NIMBLE_BASE = os.path.join(
    env.get("PROJECT_DIR", "."),
    ".pio", "libdeps", "tdeck", "NimBLE-Arduino", "src"
)

def apply_patch(filepath, old, new, label):
    if not os.path.exists(filepath):
        print(f"PATCH: {os.path.basename(filepath)} not found, skipping {label}")
        return
    with open(filepath, "r") as f:
        content = f.read()
    if old in content:
        content = content.replace(old, new)
        with open(filepath, "w") as f:
            f.write(content)
        print(f"PATCH: {label}")
    elif new in content:
        print(f"PATCH: {label} (already applied)")
    else:
        print(f"PATCH: WARNING -- {label}: expected code not found")

# Patch 1: ble_hs.c timer assert
apply_patch(
    os.path.join(NIMBLE_BASE, "nimble", "nimble", "host", "src", "ble_hs.c"),
    """    case BLE_HS_SYNC_STATE_BRINGUP:
    default:
        /* The timer should not be set in this state. */
        assert(0);
        break;""",
    """    case BLE_HS_SYNC_STATE_BRINGUP:
    default:
        /* Timer can fire during bringup due to race with host reset.
         * This is harmless — bringup will reschedule when ready. */
        break;""",
    "ble_hs.c: removed assert(0) in BRINGUP timer handler"
)

# Patch 2: NimBLEClient.cpp PHY update null guard
apply_patch(
    os.path.join(NIMBLE_BASE, "NimBLEClient.cpp"),
    """        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: {
            NimBLEConnInfo peerInfo;
            rc = ble_gap_conn_find(event->phy_updated.conn_handle, &peerInfo.m_desc);
            if (rc != 0) {
                return BLE_ATT_ERR_INVALID_HANDLE;
            }

            pClient->m_pClientCallbacks->onPhyUpdate(pClient, event->phy_updated.tx_phy, event->phy_updated.rx_phy);
            return 0;
        } // BLE_GAP_EVENT_PHY_UPDATE_COMPLETE""",
    """        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: {
            if (pClient == nullptr || pClient->m_pClientCallbacks == nullptr) {
                return 0;
            }
            NimBLEConnInfo peerInfo;
            rc = ble_gap_conn_find(event->phy_updated.conn_handle, &peerInfo.m_desc);
            if (rc != 0) {
                return BLE_ATT_ERR_INVALID_HANDLE;
            }

            pClient->m_pClientCallbacks->onPhyUpdate(pClient, event->phy_updated.tx_phy, event->phy_updated.rx_phy);
            return 0;
        } // BLE_GAP_EVENT_PHY_UPDATE_COMPLETE""",
    "NimBLEClient.cpp: added null guard in PHY update handler"
)

# Patch 3: ble_gap.c — handle BLE_ERR_CONN_ESTABLISHMENT (574) without PAwR
# The ESP32-S3 controller returns error 574 when connection establishment fails.
# NimBLE only handles this in the BLE_PERIODIC_ADV_WITH_RESPONSES path. Without it,
# 574 hits the default case (assert(0) + no cleanup), leaving master GAP state stuck
# in BLE_GAP_OP_M_CONN — permanently blocking scan and advertising.
apply_patch(
    os.path.join(NIMBLE_BASE, "nimble", "nimble", "host", "src", "ble_gap.c"),
    """#if MYNEWT_VAL(BLE_PERIODIC_ADV_WITH_RESPONSES)
        case BLE_ERR_CONN_ESTABLISHMENT:
            if (!v1_evt) {
                ble_gap_rx_conn_comp_failed(evt);
            }
            break;
#endif // MYNEWT_VAL(BLE_PERIODIC_ADV_WITH_RESPONSES)
        default:
            /* this should never happen, unless controller is broken */
            BLE_HS_LOG(INFO, "controller reported invalid error code in conn"
                             "complete event: %u", evt->status);
            assert(0);
            break;""",
    """        case BLE_ERR_CONN_ESTABLISHMENT:
            /* Connection establishment failed (e.g. peer disappeared).
             * Clean up master GAP state so scan/advertise can resume.
             * Without this, the master state stays stuck in BLE_GAP_OP_M_CONN. */
            if (ble_gap_master_in_progress()) {
                ble_gap_master_failed(BLE_HS_ECONTROLLER);
            }
            break;
        default:
            /* this should never happen, unless controller is broken */
            BLE_HS_LOG(INFO, "controller reported invalid error code in conn"
                             "complete event: %u", evt->status);
            if (ble_gap_master_in_progress()) {
                ble_gap_master_failed(BLE_HS_ECONTROLLER);
            }
            break;""",
    "ble_gap.c: handle BLE_ERR_CONN_ESTABLISHMENT (574) to prevent stuck GAP state"
)

# Patch 4: NimBLEDevice.cpp — expose host reset reason for application-level logging
# NimBLE's onReset callback logs via NIMBLE_LOGE which only goes to serial UART.
# This patch adds a global volatile int that our BLE loop can poll and log via UDP.
apply_patch(
    os.path.join(NIMBLE_BASE, "NimBLEDevice.cpp"),
    """void NimBLEDevice::onReset(int reason) {
    if (!m_synced) {
        return;
    }

    m_synced = false;

    NIMBLE_LOGE(LOG_TAG, "Host reset; reason=%d, %s", reason, NimBLEUtils::returnCodeToString(reason));
} // onReset""",
    """volatile int nimble_host_reset_reason = 0;

void NimBLEDevice::onReset(int reason) {
    if (!m_synced) {
        return;
    }

    m_synced = false;
    nimble_host_reset_reason = reason;

    NIMBLE_LOGE(LOG_TAG, "Host reset; reason=%d, %s", reason, NimBLEUtils::returnCodeToString(reason));
} // onReset""",
    "NimBLEDevice.cpp: expose host reset reason for application-level logging"
)
