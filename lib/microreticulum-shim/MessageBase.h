#pragma once

#ifndef RNS_MESSAGE_BASE_H
#define RNS_MESSAGE_BASE_H

#include "Bytes.h"

namespace RNS {

class MessageBase {
public:
    virtual ~MessageBase() = default;

    // Get the message type identifier (unique per message class)
    virtual uint16_t msgtype() const = 0;

    // Serialize message data to bytes (for wire transmission)
    virtual Bytes pack() const = 0;

    // Deserialize message data from bytes
    virtual void unpack(const Bytes& data) = 0;
};

} // namespace RNS

#endif // RNS_MESSAGE_BASE_H
