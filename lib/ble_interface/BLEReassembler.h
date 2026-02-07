/**
 * @file BLEReassembler.h
 * @brief BLE-Reticulum Protocol v2.2 fragment reassembler
 *
 * Reassembles incoming BLE fragments into complete Reticulum packets.
 * Handles timeout for incomplete reassemblies and per-peer tracking.
 * This class has no BLE dependencies and can be used for testing on native builds.
 *
 * The reassembler is keyed by peer identity (16 bytes), not MAC address,
 * to survive BLE MAC address rotation.
 *
 * Uses fixed-size pools instead of STL containers to eliminate heap fragmentation
 * on embedded systems.
 */
#pragma once

#include "BLETypes.h"
#include "BLEFragmenter.h"
#include "Bytes.h"
#include "Utilities/OS.h"

#include <functional>
#include <cstdint>
#include <cstring>

namespace RNS { namespace BLE {

// Pool sizing constants for fixed-size allocations
static constexpr size_t MAX_PENDING_REASSEMBLIES = 8;
static constexpr size_t MAX_FRAGMENTS_PER_REASSEMBLY = 32;
static constexpr size_t MAX_FRAGMENT_PAYLOAD_SIZE = 512;

class BLEReassembler {
public:
    /**
     * @brief Callback for successfully reassembled packets
     * @param peer_identity The 16-byte identity of the sending peer
     * @param packet The complete reassembled packet
     */
    using ReassemblyCallback = std::function<void(const Bytes& peer_identity, const Bytes& packet)>;

    /**
     * @brief Callback for reassembly timeout/failure
     * @param peer_identity The 16-byte identity of the peer
     * @param reason Description of the failure
     */
    using TimeoutCallback = std::function<void(const Bytes& peer_identity, const std::string& reason)>;

public:
    /**
     * @brief Construct a reassembler with default timeout
     */
    BLEReassembler();

    /**
     * @brief Set callback for successfully reassembled packets
     */
    void setReassemblyCallback(ReassemblyCallback callback);

    /**
     * @brief Set callback for reassembly timeouts/failures
     */
    void setTimeoutCallback(TimeoutCallback callback);

    /**
     * @brief Set the reassembly timeout
     * @param timeout_seconds Seconds to wait before timing out incomplete reassembly
     */
    void setTimeout(double timeout_seconds);

    /**
     * @brief Process an incoming fragment
     *
     * @param peer_identity The 16-byte identity of the sending peer
     * @param fragment The received fragment with header
     * @return true if fragment was processed successfully, false on error
     *
     * When a packet is fully reassembled, the reassembly callback is invoked.
     */
    bool processFragment(const Bytes& peer_identity, const Bytes& fragment);

    /**
     * @brief Check for timed-out reassemblies and clean them up
     *
     * Should be called periodically from the interface loop().
     * Invokes timeout callback for each expired reassembly.
     */
    void checkTimeouts();

    /**
     * @brief Get count of pending (incomplete) reassemblies
     */
    size_t pendingCount() const;

    /**
     * @brief Clear all pending reassemblies for a specific peer
     * @param peer_identity Clear only for this peer
     */
    void clearForPeer(const Bytes& peer_identity);

    /**
     * @brief Clear all pending reassemblies
     */
    void clearAll();

    /**
     * @brief Check if there's a pending reassembly for a peer
     */
    bool hasPending(const Bytes& peer_identity) const;

private:
    /**
     * @brief Information about a single received fragment (fixed-size)
     */
    struct FragmentInfo {
        uint8_t data[MAX_FRAGMENT_PAYLOAD_SIZE];
        size_t data_size = 0;
        bool received = false;

        void clear() {
            data_size = 0;
            received = false;
        }
    };

    /**
     * @brief State for a pending (incomplete) reassembly (fixed-size)
     */
    struct PendingReassembly {
        Bytes peer_identity;
        uint16_t total_fragments = 0;
        uint16_t received_count = 0;
        FragmentInfo fragments[MAX_FRAGMENTS_PER_REASSEMBLY];
        double started_at = 0.0;
        double last_activity = 0.0;

        void clear() {
            peer_identity = Bytes();
            total_fragments = 0;
            received_count = 0;
            for (size_t i = 0; i < MAX_FRAGMENTS_PER_REASSEMBLY; i++) {
                fragments[i].clear();
            }
            started_at = 0.0;
            last_activity = 0.0;
        }
    };

    /**
     * @brief Slot in the fixed-size pool for pending reassemblies
     */
    struct PendingReassemblySlot {
        bool in_use = false;
        Bytes transfer_id;  // key (peer_identity)
        PendingReassembly reassembly;

        void clear() {
            in_use = false;
            transfer_id = Bytes();
            reassembly.clear();
        }
    };

    /**
     * @brief Find a slot by peer identity
     * @return Pointer to slot or nullptr if not found
     */
    PendingReassemblySlot* findSlot(const Bytes& peer_identity);
    const PendingReassemblySlot* findSlot(const Bytes& peer_identity) const;

    /**
     * @brief Allocate a new slot for a peer
     * @return Pointer to slot or nullptr if pool is full
     */
    PendingReassemblySlot* allocateSlot(const Bytes& peer_identity);

    /**
     * @brief Concatenate all fragments in order to produce the complete packet
     */
    Bytes assembleFragments(const PendingReassembly& reassembly);

    /**
     * @brief Start a new reassembly session
     * @return true if started, false if pool is full or too many fragments
     */
    bool startReassembly(const Bytes& peer_identity, uint16_t total_fragments);

    // Fixed-size pool of pending reassemblies
    PendingReassemblySlot _pending_pool[MAX_PENDING_REASSEMBLIES];

    // Callbacks
    ReassemblyCallback _reassembly_callback = nullptr;
    TimeoutCallback _timeout_callback = nullptr;

    // Timeout configuration
    double _timeout_seconds = Timing::REASSEMBLY_TIMEOUT;
};

}} // namespace RNS::BLE
