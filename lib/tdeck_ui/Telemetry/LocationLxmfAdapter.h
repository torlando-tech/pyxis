#ifndef PYXIS_TELEMETRY_LOCATION_LXMF_ADAPTER_H
#define PYXIS_TELEMETRY_LOCATION_LXMF_ADAPTER_H

#include <cstddef>
#include <cstdint>

#include "LocationShareScheduler.h"

namespace Telemetry {

constexpr std::size_t LOCATION_LXMF_MAX_FIELDS = 2;
constexpr std::size_t LOCATION_LXMF_MAX_KEY_SIZE = 2;
constexpr std::size_t LOCATION_LXMF_MAX_VALUE_SIZE = 192;

enum class LocationDeliveryMethod : uint8_t {
    OPPORTUNISTIC,
};

struct RawLxmfField {
    uint8_t key[LOCATION_LXMF_MAX_KEY_SIZE]{};
    std::size_t key_size = 0;
    uint8_t value[LOCATION_LXMF_MAX_VALUE_SIZE]{};
    std::size_t value_size = 0;
};

struct OutboundLocationEnvelope {
    PeerId destination{};
    LocationDeliveryMethod method = LocationDeliveryMethod::OPPORTUNISTIC;
    std::size_t title_size = 0;
    std::size_t content_size = 0;
    RawLxmfField fields[LOCATION_LXMF_MAX_FIELDS]{};
    std::size_t field_count = 0;
};

enum class EnvelopeBuildResult : uint8_t {
    OK,
    INVALID_ARGUMENT,
    ENCODE_FAILED,
};

enum class DispatchResult : uint8_t {
    NO_WORK,
    CLOCK_UNAVAILABLE,
    ENCODE_FAILED,
    QUEUE_REJECTED,
    QUEUED,
    CEASE_QUEUED,
};

class LocationEnvelopeRouter {
public:
    virtual ~LocationEnvelopeRouter() {}
    // true means the implementation synchronously copied/took ownership of the
    // complete envelope. false means it retained nothing and queued nothing.
    // On true, ownership_monotonic_millis is the monotonic instant immediately
    // before the synchronous ownership copy and is strictly before the supplied
    // exclusive deadline. On false, the router retained and queued nothing.
    virtual bool queue(
        const OutboundLocationEnvelope& envelope,
        uint64_t exclusive_deadline_monotonic_millis,
        uint64_t& ownership_monotonic_millis) = 0;
};

EnvelopeBuildResult buildOutboundLocationEnvelope(
    const ShareWork& work,
    const LocationTelemetry& current_location,
    uint64_t wall_now_millis,
    OutboundLocationEnvelope& output);

// Polls and resolves exactly one scheduler lease. Scheduler acknowledgement is
// positive only after queue() confirms synchronous ownership.
DispatchResult dispatchLocationShare(
    LocationShareScheduler& scheduler,
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis,
    bool current_location_valid,
    const LocationTelemetry& current_location,
    LocationEnvelopeRouter& router);

}  // namespace Telemetry

#endif
