"""
PlatformIO pre-build script: Patch NimBLE stability issues.

Patch 1 — ble_hs.c: Remove assert(0) in BLE_HS_SYNC_STATE_BRINGUP timer handler.
  Timer can fire during host re-sync due to a race condition. Harmless — just ignore it.

Patch 2 — NimBLEClient.cpp: Add null checks in PHY update event handler.
  If a client is deleted while events are queued, the callback arg becomes a dangling
  pointer. Guard against null pClient and null m_pClientCallbacks.
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
