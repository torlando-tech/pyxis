#ifndef PYXIS_TELEMETRY_LOCATION_MESSAGE_POLICY_H
#define PYXIS_TELEMETRY_LOCATION_MESSAGE_POLICY_H

#include <cstddef>
#include <cstdint>

#include "LocationShareState.h"

namespace Telemetry {

struct RawLocationField {
    RawLocationField() : present(false), raw_value() {}
    bool present;
    BinaryView raw_value;
};

struct TextField {
    TextField() : present(false), data(nullptr), size(0) {}
    TextField(bool is_present, const uint8_t* bytes, std::size_t length)
        : present(is_present), data(bytes), size(length) {}
    bool present;
    const uint8_t* data;
    std::size_t size;
};

struct InboundLocationMessage {
    InboundLocationMessage()
        : authenticated_sender(), telemetry(), custom_meta(), title(), content(),
          received_at_millis(0) {}
    PeerId authenticated_sender;
    RawLocationField telemetry;
    RawLocationField custom_meta;
    TextField title;
    TextField content;
    uint64_t received_at_millis;
};

enum class LocationMessageKind : uint8_t {
    NOT_LOCATION,
    VALID_LOCATION,
    VALID_CEASE,
    MALFORMED_LOCATION,
};

struct LocationMessageDecision {
    LocationMessageDecision()
        : kind(LocationMessageKind::NOT_LOCATION), authenticated_sender(),
          location(), meta(), received_at_millis(0), apply_location(false),
          persist(true), route(true), notify(true), drop(false),
          log_malformed(false) {}
    LocationMessageKind kind;
    PeerId authenticated_sender;
    LocationTelemetry location;
    CustomLocationMeta meta;
    uint64_t received_at_millis;
    bool apply_location;
    bool persist;
    bool route;
    bool notify;
    bool drop;
    bool log_malformed;
};

LocationMessageDecision classifyInboundLocationMessage(
    const InboundLocationMessage& message);

class LocationMessageEffects {
public:
    virtual ~LocationMessageEffects() {}
    virtual PeerLocationResult applyLocation(
        const PeerId& authenticated_sender,
        const LocationTelemetry& location,
        const CustomLocationMeta& meta,
        uint64_t received_at_millis) = 0;
    virtual bool persistMessage() = 0;
    virtual void routeMessage() = 0;
    virtual void notifyMessage() = 0;
    virtual void logMalformedLocation() = 0;
};

struct LocationMessageExecution {
    LocationMessageExecution()
        : location_result(PeerLocationResult::INVALID_ARGUMENT),
          location_attempted(false), persistence_attempted(false),
          persisted(false), routed(false), notified(false), malformed_logged(false) {}
    PeerLocationResult location_result;
    bool location_attempted;
    bool persistence_attempted;
    bool persisted;
    bool routed;
    bool notified;
    bool malformed_logged;
};

// Side effects are deliberately ordered: malformed log, location state, durable
// chat persistence, in-memory/UI routing, then notification. A persistence
// failure stops routing and notification, matching the inbound chat contract.
LocationMessageExecution executeLocationMessageDecision(
    const LocationMessageDecision& decision,
    LocationMessageEffects& effects);

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_MESSAGE_POLICY_H
