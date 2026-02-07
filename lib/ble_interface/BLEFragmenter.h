/**
 * @file BLEFragmenter.h
 * @brief BLE-Reticulum Protocol v2.2 packet fragmenter
 *
 * Fragments outgoing Reticulum packets into BLE-sized chunks with the v2.2
 * 5-byte header format. This class has no BLE dependencies and can be used
 * for testing on native builds.
 *
 * Fragment Header Format (5 bytes):
 *   Byte 0:     Type (0x01=START, 0x02=CONTINUE, 0x03=END)
 *   Bytes 1-2:  Sequence number (big-endian uint16_t)
 *   Bytes 3-4:  Total fragments (big-endian uint16_t)
 *   Bytes 5+:   Payload data
 */
#pragma once

#include "BLETypes.h"
#include "Bytes.h"

#include <vector>
#include <cstdint>

namespace RNS { namespace BLE {

class BLEFragmenter {
public:
    /**
     * @brief Construct a fragmenter with specified MTU
     * @param mtu The negotiated BLE MTU (default: minimum BLE MTU of 23)
     */
    explicit BLEFragmenter(size_t mtu = MTU::MINIMUM);

    /**
     * @brief Set the MTU for fragmentation calculations
     *
     * Call this when MTU is renegotiated with a peer.
     * @param mtu The new MTU value
     */
    void setMTU(size_t mtu);

    /**
     * @brief Get the current MTU
     */
    size_t getMTU() const { return _mtu; }

    /**
     * @brief Get the maximum payload size per fragment (MTU - HEADER_SIZE)
     */
    size_t getPayloadSize() const { return _payload_size; }

    /**
     * @brief Check if a packet needs fragmentation
     * @param data The packet to check
     * @return true if packet exceeds single fragment payload capacity
     */
    bool needsFragmentation(const Bytes& data) const;

    /**
     * @brief Calculate number of fragments needed for a packet
     * @param data_size Size of the data to fragment
     * @return Number of fragments needed (minimum 1)
     */
    uint16_t calculateFragmentCount(size_t data_size) const;

    /**
     * @brief Fragment a packet into BLE-sized chunks
     *
     * @param data The complete packet to fragment
     * @param sequence_base Starting sequence number (default 0)
     * @return Vector of fragments, each with the 5-byte header prepended
     *
     * For a single-fragment packet, returns one fragment with type=END.
     * For multi-fragment packets:
     *   - First fragment has type=START
     *   - Middle fragments have type=CONTINUE
     *   - Last fragment has type=END
     */
    std::vector<Bytes> fragment(const Bytes& data, uint16_t sequence_base = 0);

    /**
     * @brief Create a single fragment with proper header
     *
     * @param type Fragment type (START, CONTINUE, END)
     * @param sequence Sequence number for this fragment
     * @param total_fragments Total number of fragments in this message
     * @param payload The fragment payload data
     * @return Complete fragment with 5-byte header prepended
     */
    static Bytes createFragment(Fragment::Type type, uint16_t sequence,
                                 uint16_t total_fragments, const Bytes& payload);

    /**
     * @brief Parse the header from a received fragment
     *
     * @param fragment The received fragment data (must be at least HEADER_SIZE bytes)
     * @param type Output: fragment type
     * @param sequence Output: sequence number
     * @param total_fragments Output: total fragment count
     * @return true if header is valid and was parsed successfully
     */
    static bool parseHeader(const Bytes& fragment, Fragment::Type& type,
                            uint16_t& sequence, uint16_t& total_fragments);

    /**
     * @brief Extract payload from a fragment (removes header)
     *
     * @param fragment The complete fragment with header
     * @return The payload portion (empty if fragment is too small)
     */
    static Bytes extractPayload(const Bytes& fragment);

    /**
     * @brief Validate a fragment header
     *
     * @param fragment The fragment to validate
     * @return true if the fragment has a valid header
     */
    static bool isValidFragment(const Bytes& fragment);

private:
    size_t _mtu;
    size_t _payload_size;
};

}} // namespace RNS::BLE
