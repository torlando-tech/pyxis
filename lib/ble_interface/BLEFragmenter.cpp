/**
 * @file BLEFragmenter.cpp
 * @brief BLE-Reticulum Protocol v2.2 packet fragmenter implementation
 */

#include "BLEFragmenter.h"
#include "Log.h"

namespace RNS { namespace BLE {

BLEFragmenter::BLEFragmenter(size_t mtu) {
    setMTU(mtu);
}

void BLEFragmenter::setMTU(size_t mtu) {
    // Ensure MTU is at least the minimum
    _mtu = (mtu >= MTU::MINIMUM) ? mtu : MTU::MINIMUM;

    // Calculate payload size (MTU minus header)
    if (_mtu > Fragment::HEADER_SIZE) {
        _payload_size = _mtu - Fragment::HEADER_SIZE;
    } else {
        _payload_size = 0;
        WARNING("BLEFragmenter: MTU too small for fragmentation");
    }
}

bool BLEFragmenter::needsFragmentation(const Bytes& data) const {
    return data.size() > _payload_size;
}

uint16_t BLEFragmenter::calculateFragmentCount(size_t data_size) const {
    if (_payload_size == 0) return 0;
    if (data_size == 0) return 1;  // Empty data still produces one fragment
    return static_cast<uint16_t>((data_size + _payload_size - 1) / _payload_size);
}

std::vector<Bytes> BLEFragmenter::fragment(const Bytes& data, uint16_t sequence_base) {
    std::vector<Bytes> fragments;

    if (_payload_size == 0) {
        ERROR("BLEFragmenter: Cannot fragment with zero payload size");
        return fragments;
    }

    // Handle empty data case
    if (data.size() == 0) {
        // Single empty END fragment
        fragments.push_back(createFragment(Fragment::END, sequence_base, 1, Bytes()));
        return fragments;
    }

    uint16_t total_fragments = calculateFragmentCount(data.size());

    // Pre-allocate vector to avoid incremental reallocations
    fragments.reserve(total_fragments);

    size_t offset = 0;

    for (uint16_t i = 0; i < total_fragments; i++) {
        // Calculate payload size for this fragment
        size_t remaining = data.size() - offset;
        size_t chunk_size = (remaining < _payload_size) ? remaining : _payload_size;

        // Extract payload chunk
        Bytes payload(data.data() + offset, chunk_size);
        offset += chunk_size;

        // Determine fragment type
        Fragment::Type type;
        if (total_fragments == 1) {
            // Single fragment - use END type
            type = Fragment::END;
        } else if (i == 0) {
            // First of multiple fragments
            type = Fragment::START;
        } else if (i == total_fragments - 1) {
            // Last fragment
            type = Fragment::END;
        } else {
            // Middle fragment
            type = Fragment::CONTINUE;
        }

        uint16_t sequence = sequence_base + i;
        fragments.push_back(createFragment(type, sequence, total_fragments, payload));
    }

    {
        char buf[80];
        snprintf(buf, sizeof(buf), "BLEFragmenter: Fragmented %zu bytes into %zu fragments",
                 data.size(), fragments.size());
        TRACE(buf);
    }

    return fragments;
}

Bytes BLEFragmenter::createFragment(Fragment::Type type, uint16_t sequence,
                                     uint16_t total_fragments, const Bytes& payload) {
    // Allocate buffer for header + payload
    size_t total_size = Fragment::HEADER_SIZE + payload.size();
    Bytes fragment(total_size);
    uint8_t* ptr = fragment.writable(total_size);
    fragment.resize(total_size);

    // Byte 0: Type
    ptr[0] = static_cast<uint8_t>(type);

    // Bytes 1-2: Sequence number (big-endian)
    ptr[1] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
    ptr[2] = static_cast<uint8_t>(sequence & 0xFF);

    // Bytes 3-4: Total fragments (big-endian)
    ptr[3] = static_cast<uint8_t>((total_fragments >> 8) & 0xFF);
    ptr[4] = static_cast<uint8_t>(total_fragments & 0xFF);

    // Bytes 5+: Payload
    if (payload.size() > 0) {
        memcpy(ptr + Fragment::HEADER_SIZE, payload.data(), payload.size());
    }

    return fragment;
}

bool BLEFragmenter::parseHeader(const Bytes& fragment, Fragment::Type& type,
                                 uint16_t& sequence, uint16_t& total_fragments) {
    if (fragment.size() < Fragment::HEADER_SIZE) {
        return false;
    }

    const uint8_t* ptr = fragment.data();

    // Byte 0: Type
    uint8_t type_byte = ptr[0];
    if (type_byte != Fragment::START &&
        type_byte != Fragment::CONTINUE &&
        type_byte != Fragment::END) {
        return false;
    }
    type = static_cast<Fragment::Type>(type_byte);

    // Bytes 1-2: Sequence number (big-endian)
    sequence = (static_cast<uint16_t>(ptr[1]) << 8) | static_cast<uint16_t>(ptr[2]);

    // Bytes 3-4: Total fragments (big-endian)
    total_fragments = (static_cast<uint16_t>(ptr[3]) << 8) | static_cast<uint16_t>(ptr[4]);

    // Validate total_fragments is non-zero
    if (total_fragments == 0) {
        return false;
    }

    // Validate sequence < total_fragments
    if (sequence >= total_fragments) {
        return false;
    }

    return true;
}

Bytes BLEFragmenter::extractPayload(const Bytes& fragment) {
    if (fragment.size() <= Fragment::HEADER_SIZE) {
        return Bytes();
    }

    return Bytes(fragment.data() + Fragment::HEADER_SIZE,
                 fragment.size() - Fragment::HEADER_SIZE);
}

bool BLEFragmenter::isValidFragment(const Bytes& fragment) {
    Fragment::Type type;
    uint16_t sequence, total;
    return parseHeader(fragment, type, sequence, total);
}

}} // namespace RNS::BLE
