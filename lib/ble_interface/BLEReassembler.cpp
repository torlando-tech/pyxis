/**
 * @file BLEReassembler.cpp
 * @brief BLE-Reticulum Protocol v2.2 fragment reassembler implementation
 *
 * Uses fixed-size pools instead of STL containers to eliminate heap fragmentation.
 */

#include "BLEReassembler.h"
#include "Log.h"

namespace RNS { namespace BLE {

BLEReassembler::BLEReassembler() {
    // Default timeout from protocol spec
    _timeout_seconds = Timing::REASSEMBLY_TIMEOUT;
    // Initialize pool
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        _pending_pool[i].clear();
    }
}

BLEReassembler::PendingReassemblySlot* BLEReassembler::findSlot(const Bytes& peer_identity) {
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        if (_pending_pool[i].in_use && _pending_pool[i].transfer_id == peer_identity) {
            return &_pending_pool[i];
        }
    }
    return nullptr;
}

const BLEReassembler::PendingReassemblySlot* BLEReassembler::findSlot(const Bytes& peer_identity) const {
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        if (_pending_pool[i].in_use && _pending_pool[i].transfer_id == peer_identity) {
            return &_pending_pool[i];
        }
    }
    return nullptr;
}

BLEReassembler::PendingReassemblySlot* BLEReassembler::allocateSlot(const Bytes& peer_identity) {
    // First check if slot already exists for this peer
    PendingReassemblySlot* existing = findSlot(peer_identity);
    if (existing) {
        return existing;
    }

    // Find a free slot
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        if (!_pending_pool[i].in_use) {
            _pending_pool[i].in_use = true;
            _pending_pool[i].transfer_id = peer_identity;
            return &_pending_pool[i];
        }
    }
    return nullptr;  // Pool is full
}

size_t BLEReassembler::pendingCount() const {
    size_t count = 0;
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        if (_pending_pool[i].in_use) {
            count++;
        }
    }
    return count;
}

void BLEReassembler::setReassemblyCallback(ReassemblyCallback callback) {
    _reassembly_callback = callback;
}

void BLEReassembler::setTimeoutCallback(TimeoutCallback callback) {
    _timeout_callback = callback;
}

void BLEReassembler::setTimeout(double timeout_seconds) {
    _timeout_seconds = timeout_seconds;
}

bool BLEReassembler::processFragment(const Bytes& peer_identity, const Bytes& fragment) {
    // Validate fragment
    if (!BLEFragmenter::isValidFragment(fragment)) {
        TRACE("BLEReassembler: Invalid fragment header");
        return false;
    }

    // Parse header
    Fragment::Type type;
    uint16_t sequence;
    uint16_t total_fragments;
    if (!BLEFragmenter::parseHeader(fragment, type, sequence, total_fragments)) {
        TRACE("BLEReassembler: Failed to parse fragment header");
        return false;
    }

    double now = Utilities::OS::time();

    // Handle START fragment - begins a new reassembly
    if (type == Fragment::START) {
        // Clear any existing incomplete reassembly for this peer
        PendingReassemblySlot* existing = findSlot(peer_identity);
        if (existing) {
            TRACE("BLEReassembler: Discarding incomplete reassembly for new START");
            existing->clear();
        }

        // Start new reassembly
        if (!startReassembly(peer_identity, total_fragments)) {
            return false;
        }
    }

    // Look up pending reassembly
    PendingReassemblySlot* slot = findSlot(peer_identity);
    if (!slot) {
        // No pending reassembly and this isn't a START
        if (type != Fragment::START) {
            // For single-fragment packets (type=END, total=1, seq=0), start immediately
            if (type == Fragment::END && total_fragments == 1 && sequence == 0) {
                if (!startReassembly(peer_identity, total_fragments)) {
                    return false;
                }
                slot = findSlot(peer_identity);
            } else {
                TRACE("BLEReassembler: Received fragment without START, discarding");
                return false;
            }
        } else {
            slot = findSlot(peer_identity);
        }
    }

    if (!slot) {
        ERROR("BLEReassembler: Failed to find/create reassembly session");
        return false;
    }

    PendingReassembly& reassembly = slot->reassembly;

    // Validate total_fragments matches
    if (total_fragments != reassembly.total_fragments) {
        char buf[80];
        snprintf(buf, sizeof(buf), "BLEReassembler: Fragment total mismatch, expected %u got %u",
                 reassembly.total_fragments, total_fragments);
        TRACE(buf);
        return false;
    }

    // Validate sequence is in range
    if (sequence >= reassembly.total_fragments) {
        char buf[64];
        snprintf(buf, sizeof(buf), "BLEReassembler: Sequence out of range: %u", sequence);
        TRACE(buf);
        return false;
    }

    // Check for duplicate
    if (reassembly.fragments[sequence].received) {
        char buf[64];
        snprintf(buf, sizeof(buf), "BLEReassembler: Duplicate fragment %u", sequence);
        TRACE(buf);
        // Still update last_activity to keep session alive
        reassembly.last_activity = now;
        return true;  // Not an error, just duplicate
    }

    // Store fragment payload into fixed-size buffer
    Bytes payload = BLEFragmenter::extractPayload(fragment);
    if (payload.size() > MAX_FRAGMENT_PAYLOAD_SIZE) {
        char buf[80];
        snprintf(buf, sizeof(buf), "BLEReassembler: Fragment payload too large: %zu > %zu",
                 payload.size(), MAX_FRAGMENT_PAYLOAD_SIZE);
        WARNING(buf);
        return false;
    }
    memcpy(reassembly.fragments[sequence].data, payload.data(), payload.size());
    reassembly.fragments[sequence].data_size = payload.size();
    reassembly.fragments[sequence].received = true;
    reassembly.received_count++;
    reassembly.last_activity = now;

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "BLEReassembler: Received fragment %u/%u", sequence + 1, reassembly.total_fragments);
        TRACE(buf);
    }

    // Check if complete
    if (reassembly.received_count == reassembly.total_fragments) {
        // Assemble complete packet
        Bytes complete_packet = assembleFragments(reassembly);

        {
            char buf[64];
            snprintf(buf, sizeof(buf), "BLEReassembler: Completed reassembly, %zu bytes", complete_packet.size());
            TRACE(buf);
        }

        // Remove from pending before callback (callback might trigger new data)
        Bytes identity_copy = reassembly.peer_identity;
        slot->clear();

        // Invoke callback
        if (_reassembly_callback) {
            _reassembly_callback(identity_copy, complete_packet);
        }
    }

    return true;
}

void BLEReassembler::checkTimeouts() {
    double now = Utilities::OS::time();

    // Find and clean up expired reassemblies
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        if (!_pending_pool[i].in_use) {
            continue;
        }

        PendingReassembly& reassembly = _pending_pool[i].reassembly;
        double age = now - reassembly.started_at;

        if (age > _timeout_seconds) {
            {
                char buf[80];
                snprintf(buf, sizeof(buf), "BLEReassembler: Timeout waiting for fragments, received %u/%u",
                         reassembly.received_count, reassembly.total_fragments);
                WARNING(buf);
            }

            // Copy identity before clearing
            Bytes peer_identity = _pending_pool[i].transfer_id;

            // Clear the slot
            _pending_pool[i].clear();

            // Invoke timeout callback after clearing (callback might start new reassembly)
            if (_timeout_callback) {
                _timeout_callback(peer_identity, "Reassembly timeout");
            }
        }
    }
}

void BLEReassembler::clearForPeer(const Bytes& peer_identity) {
    PendingReassemblySlot* slot = findSlot(peer_identity);
    if (slot) {
        TRACE("BLEReassembler: Clearing pending reassembly for peer");
        slot->clear();
    }
}

void BLEReassembler::clearAll() {
    char buf[64];
    snprintf(buf, sizeof(buf), "BLEReassembler: Clearing all pending reassemblies (%zu sessions)", pendingCount());
    TRACE(buf);
    for (size_t i = 0; i < MAX_PENDING_REASSEMBLIES; i++) {
        _pending_pool[i].clear();
    }
}

bool BLEReassembler::hasPending(const Bytes& peer_identity) const {
    return findSlot(peer_identity) != nullptr;
}

bool BLEReassembler::startReassembly(const Bytes& peer_identity, uint16_t total_fragments) {
    // Validate fragment count fits in fixed-size array
    if (total_fragments > MAX_FRAGMENTS_PER_REASSEMBLY) {
        char buf[80];
        snprintf(buf, sizeof(buf), "BLEReassembler: Too many fragments: %u > %zu",
                 total_fragments, MAX_FRAGMENTS_PER_REASSEMBLY);
        WARNING(buf);
        return false;
    }

    // Allocate a slot (reuses existing or finds free)
    PendingReassemblySlot* slot = allocateSlot(peer_identity);
    if (!slot) {
        WARNING("BLEReassembler: Pool full, cannot start new reassembly");
        return false;
    }

    double now = Utilities::OS::time();

    // Initialize the reassembly state
    PendingReassembly& reassembly = slot->reassembly;
    reassembly.clear();  // Clear any old data
    reassembly.peer_identity = peer_identity;
    reassembly.total_fragments = total_fragments;
    reassembly.received_count = 0;
    reassembly.started_at = now;
    reassembly.last_activity = now;

    char buf[64];
    snprintf(buf, sizeof(buf), "BLEReassembler: Starting reassembly for %u fragments", total_fragments);
    TRACE(buf);
    return true;
}

Bytes BLEReassembler::assembleFragments(const PendingReassembly& reassembly) {
    // Calculate total size
    size_t total_size = 0;
    for (size_t i = 0; i < reassembly.total_fragments; i++) {
        total_size += reassembly.fragments[i].data_size;
    }

    // Allocate result buffer
    Bytes result(total_size);
    uint8_t* ptr = result.writable(total_size);
    result.resize(total_size);

    // Concatenate fragments in order
    size_t offset = 0;
    for (size_t i = 0; i < reassembly.total_fragments; i++) {
        const FragmentInfo& frag = reassembly.fragments[i];
        if (frag.data_size > 0) {
            memcpy(ptr + offset, frag.data, frag.data_size);
            offset += frag.data_size;
        }
    }

    return result;
}

}} // namespace RNS::BLE
