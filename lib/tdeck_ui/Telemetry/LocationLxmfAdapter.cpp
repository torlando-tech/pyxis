#include "LocationLxmfAdapter.h"

#include <limits>

namespace Telemetry {
namespace {

bool toSignedMillis(uint64_t value, int64_t& output) {
    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (value > maximum) return false;
    output = static_cast<int64_t>(value);
    return true;
}

EnvelopeBuildResult encodeTelemetryField(
    const LocationTelemetry& location,
    RawLxmfField& field) {
    uint8_t inner[128]{};
    std::size_t inner_size = 0;
    if (encodeLocationTelemetry(location, inner, sizeof(inner), inner_size) !=
        EncodeResult::OK) {
        return EnvelopeBuildResult::ENCODE_FAILED;
    }
    field.key[0] = FIELD_TELEMETRY;
    field.key_size = 1;
    if (wrapLxmfBinaryFieldValue(
            inner, inner_size, field.value, sizeof(field.value),
            field.value_size) != FieldValueResult::OK) {
        return EnvelopeBuildResult::ENCODE_FAILED;
    }
    return EnvelopeBuildResult::OK;
}

}  // namespace

EnvelopeBuildResult buildOutboundLocationEnvelope(
    const ShareWork& work,
    const LocationTelemetry& current_location,
    uint64_t wall_now_millis,
    OutboundLocationEnvelope& output) {
    OutboundLocationEnvelope candidate{};
    candidate.destination = work.peer;

    LocationTelemetry wire_location = current_location;
    CustomLocationMeta meta{};
    if (work.type == ShareWorkType::CEASE) {
        wire_location = LocationTelemetry{};
        wire_location.timestamp_seconds = wall_now_millis / 1000ULL;
        wire_location.sensor_timestamp_seconds =
            wire_location.timestamp_seconds;
        meta.has_cease = true;
        meta.cease = true;
    } else if (!isValidPeerLocationInput(
                   wire_location, CustomLocationMeta{}, wall_now_millis)) {
        return EnvelopeBuildResult::INVALID_ARGUMENT;
    }

    if (encodeTelemetryField(wire_location, candidate.fields[0]) !=
        EnvelopeBuildResult::OK) {
        return EnvelopeBuildResult::ENCODE_FAILED;
    }
    candidate.field_count = 1;

    const bool has_radius =
        work.has_approx_radius || work.approx_radius_meters != 0;
    const bool has_meta = meta.has_cease || work.has_expiry || has_radius;
    if (!has_meta) {
        output = candidate;
        return EnvelopeBuildResult::OK;
    }

    if (work.has_expiry) {
        if (!toSignedMillis(work.expires_at_millis, meta.expires_millis)) {
            return EnvelopeBuildResult::INVALID_ARGUMENT;
        }
        meta.has_expires = true;
    }
    if (has_radius) {
        if (work.approx_radius_meters < 0) {
            return EnvelopeBuildResult::INVALID_ARGUMENT;
        }
        meta.has_approx_radius = true;
        meta.approx_radius_meters = work.approx_radius_meters;
    }
    if (meta.has_cease) {
        if (!toSignedMillis(wall_now_millis, meta.timestamp_millis)) {
            return EnvelopeBuildResult::INVALID_ARGUMENT;
        }
        meta.has_timestamp = true;
    }

    RawLxmfField& meta_field = candidate.fields[1];
    meta_field.key[0] = 0xccU;  // MessagePack uint8
    meta_field.key[1] = FIELD_CUSTOM_META;
    meta_field.key_size = 2;
    uint8_t encoded_meta[128]{};
    std::size_t encoded_meta_size = 0;
    const CustomMetaResult meta_result = encodeCustomLocationMeta(
        meta, encoded_meta, sizeof(encoded_meta), encoded_meta_size);
    if (meta_result != CustomMetaResult::OK) {
        return EnvelopeBuildResult::ENCODE_FAILED;
    }
    if (wrapLxmfBinaryFieldValue(
            encoded_meta, encoded_meta_size,
            meta_field.value, sizeof(meta_field.value),
            meta_field.value_size) != FieldValueResult::OK) {
        return EnvelopeBuildResult::ENCODE_FAILED;
    }
    candidate.field_count = 2;
    output = candidate;
    return EnvelopeBuildResult::OK;
}

DispatchResult dispatchLocationShare(
    LocationShareScheduler& scheduler,
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis,
    bool current_location_valid,
    const LocationTelemetry& current_location,
    LocationEnvelopeRouter& router) {
    ShareWork work{};
    const SharePollResult poll_result = scheduler.poll(
        wall_now_millis, monotonic_now_millis, current_location_valid, work);
    if (poll_result == SharePollResult::NO_WORK) return DispatchResult::NO_WORK;
    if (poll_result == SharePollResult::CLOCK_UNAVAILABLE) {
        return DispatchResult::CLOCK_UNAVAILABLE;
    }

    OutboundLocationEnvelope envelope{};
    const EnvelopeBuildResult build_result = buildOutboundLocationEnvelope(
        work, current_location, wall_now_millis, envelope);
    if (build_result != EnvelopeBuildResult::OK) {
        scheduler.acknowledge(work.peer, work.token, false,
                              wall_now_millis, monotonic_now_millis);
        return DispatchResult::ENCODE_FAILED;
    }

    uint64_t ownership_monotonic_millis = 0;
    const bool accepted = router.queue(
        envelope, work.ack_deadline_monotonic_millis,
        ownership_monotonic_millis);
    const ShareAckResult ack = scheduler.acknowledge(
        work.peer, work.token, accepted,
        wall_now_millis,
        accepted ? ownership_monotonic_millis : monotonic_now_millis);
    if (!accepted) return DispatchResult::QUEUE_REJECTED;
    if (ack == ShareAckResult::CEASED) return DispatchResult::CEASE_QUEUED;
    if (ack == ShareAckResult::ACCEPTED) return DispatchResult::QUEUED;
    // A synchronous router must return before the scheduler lease expires.
    // If that invariant is violated, never claim queue success to the caller.
    return DispatchResult::CLOCK_UNAVAILABLE;
}

}  // namespace Telemetry
