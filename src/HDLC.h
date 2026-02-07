#pragma once

#include "Bytes.h"

#include <stdint.h>

namespace RNS {

/**
 * HDLC framing implementation for TCP transport.
 *
 * Matches Python RNS TCPInterface HDLC framing (RNS/Interfaces/TCPInterface.py).
 *
 * Wire format: [FLAG][escaped_payload][FLAG]
 * Where:
 *   FLAG = 0x7E
 *   ESC  = 0x7D
 *
 * Escape sequences:
 *   0x7E in payload -> 0x7D 0x5E
 *   0x7D in payload -> 0x7D 0x5D
 */
class HDLC {
public:
    static constexpr uint8_t FLAG = 0x7E;
    static constexpr uint8_t ESC = 0x7D;
    static constexpr uint8_t ESC_MASK = 0x20;

    /**
     * Escape data for transmission.
     * Order matches Python RNS: escape ESC first, then FLAG.
     *
     * @param data Raw payload data
     * @return Escaped data (without FLAG bytes)
     */
    static Bytes escape(const Bytes& data) {
        Bytes result;
        result.reserve(data.size() * 2);  // worst case: every byte needs escaping

        for (size_t i = 0; i < data.size(); ++i) {
            uint8_t byte = data.data()[i];
            if (byte == ESC) {
                // Escape ESC byte: 0x7D -> 0x7D 0x5D
                result.append(ESC);
                result.append(static_cast<uint8_t>(ESC ^ ESC_MASK));
            } else if (byte == FLAG) {
                // Escape FLAG byte: 0x7E -> 0x7D 0x5E
                result.append(ESC);
                result.append(static_cast<uint8_t>(FLAG ^ ESC_MASK));
            } else {
                result.append(byte);
            }
        }
        return result;
    }

    /**
     * Unescape received data.
     *
     * @param data Escaped payload data (without FLAG bytes)
     * @return Unescaped data, or empty Bytes on error
     */
    static Bytes unescape(const Bytes& data) {
        Bytes result;
        result.reserve(data.size());

        bool in_escape = false;
        for (size_t i = 0; i < data.size(); ++i) {
            uint8_t byte = data.data()[i];
            if (in_escape) {
                // XOR with ESC_MASK to restore original byte
                result.append(static_cast<uint8_t>(byte ^ ESC_MASK));
                in_escape = false;
            } else if (byte == ESC) {
                in_escape = true;
            } else {
                result.append(byte);
            }
        }

        // If we ended mid-escape, that's an error
        if (in_escape) {
            return Bytes();  // empty = error
        }
        return result;
    }

    /**
     * Create a framed packet for transmission.
     *
     * @param data Raw payload data
     * @return Framed data: [FLAG][escaped_data][FLAG]
     */
    static Bytes frame(const Bytes& data) {
        Bytes escaped = escape(data);
        Bytes framed;
        framed.reserve(escaped.size() + 2);
        framed.append(FLAG);
        framed.append(escaped);
        framed.append(FLAG);
        return framed;
    }
};

}  // namespace RNS
