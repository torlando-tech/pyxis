#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "Telemetry/LocationLxmfAdapter.h"

namespace {
int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

Telemetry::PeerId peer(uint8_t seed) {
    Telemetry::PeerId value{};
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) value.bytes[i] = static_cast<uint8_t>(seed + i);
    return value;
}

Telemetry::LocationTelemetry location(uint64_t seconds) {
    Telemetry::LocationTelemetry value{};
    value.latitude_e6 = 37774900;
    value.longitude_e6 = -122419400;
    value.altitude_cm = 1600;
    value.speed_centi_kmh = 1234;
    value.bearing_cdeg = 4200;
    value.accuracy_cm = 350;
    value.timestamp_seconds = seconds;
    value.sensor_timestamp_seconds = seconds;
    return value;
}

class FakeRouter : public Telemetry::LocationEnvelopeRouter {
public:
    bool accepted = true;
    uint64_t ownership_time = 0;
    int calls = 0;
    Telemetry::OutboundLocationEnvelope envelope{};
    bool queue(const Telemetry::OutboundLocationEnvelope& value,
               uint64_t deadline,
               uint64_t& ownership) override {
        ++calls;
        envelope = value;
        ownership = ownership_time == 0
                        ? deadline - Telemetry::ACKNOWLEDGEMENT_LEASE_MILLIS
                        : ownership_time;
        if (ownership >= deadline) return false;
        return accepted;
    }
};

void exactLocationEnvelope() {
    Telemetry::ShareWork work{};
    work.peer = peer(7);
    work.type = Telemetry::ShareWorkType::LOCATION;
    work.has_expiry = true;
    work.expires_at_millis = 1700000900000ULL;
    work.approx_radius_meters = 25;
    Telemetry::OutboundLocationEnvelope envelope{};
    CHECK(Telemetry::buildOutboundLocationEnvelope(work, location(1700000000ULL), 1700000000123ULL, envelope) == Telemetry::EnvelopeBuildResult::OK);
    CHECK(std::memcmp(envelope.destination.bytes, work.peer.bytes, Telemetry::PEER_ID_SIZE) == 0);
    CHECK(envelope.method == Telemetry::LocationDeliveryMethod::OPPORTUNISTIC);
    CHECK(envelope.title_size == 0 && envelope.content_size == 0);
    CHECK(envelope.field_count == 2);
    CHECK(envelope.fields[0].key_size == 1 && envelope.fields[0].key[0] == 0x02);
    CHECK(envelope.fields[1].key_size == 2 && envelope.fields[1].key[0] == 0xcc && envelope.fields[1].key[1] == 0xfd);
    Telemetry::BinaryView inner{};
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(envelope.fields[0].value, envelope.fields[0].value_size, inner) == Telemetry::FieldValueResult::OK);
    Telemetry::LocationTelemetry decoded{};
    CHECK(Telemetry::decodeLocationTelemetry(inner.data, inner.size, decoded) == Telemetry::DecodeResult::OK);
    CHECK(decoded.latitude_e6 == 37774900 && decoded.timestamp_seconds == 1700000000ULL);
    Telemetry::BinaryView encoded_meta{};
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
              envelope.fields[1].value, envelope.fields[1].value_size,
              encoded_meta) == Telemetry::FieldValueResult::OK);
    Telemetry::CustomLocationMeta meta{};
    CHECK(Telemetry::decodeCustomLocationMeta(
              encoded_meta.data, encoded_meta.size, meta) ==
          Telemetry::CustomMetaResult::OK);
    CHECK(!meta.has_cease && meta.has_expires && meta.expires_millis == 1700000900000LL);
    CHECK(meta.has_approx_radius && meta.approx_radius_meters == 25);
    CHECK(!meta.has_timestamp);
}

void acceptanceAndRetryAreHonest() {
    Telemetry::LocationShareScheduler scheduler;
    Telemetry::ShareStartOptions options{};
    options.duration = Telemetry::ShareDuration::INDEFINITE;
    CHECK(scheduler.start(peer(1), options, 100000) == Telemetry::ShareSessionResult::STARTED);
    FakeRouter router;
    router.accepted = false;
    Telemetry::DispatchResult result = Telemetry::dispatchLocationShare(scheduler, 100000, 5000, true, location(100), router);
    CHECK(result == Telemetry::DispatchResult::QUEUE_REJECTED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(1), session) && !session.has_sent && !session.awaiting_ack);
    CHECK(session.next_attempt_millis == 100000 + Telemetry::INITIAL_RETRY_MILLIS);
    CHECK(Telemetry::dispatchLocationShare(scheduler, session.next_attempt_millis - 1, 5001, true, location(100), router) == Telemetry::DispatchResult::NO_WORK);
    router.accepted = true;
    CHECK(Telemetry::dispatchLocationShare(scheduler, session.next_attempt_millis, 5002, true, location(105), router) == Telemetry::DispatchResult::QUEUED);
    CHECK(scheduler.get(peer(1), session) && session.has_sent && !session.awaiting_ack);
}

void ceaseShapeAndOwnership() {
    Telemetry::LocationShareScheduler scheduler;
    Telemetry::ShareStartOptions options{};
    options.duration = Telemetry::ShareDuration::INDEFINITE;
    CHECK(scheduler.start(peer(3), options, 1000) == Telemetry::ShareSessionResult::STARTED);
    FakeRouter router;
    CHECK(Telemetry::dispatchLocationShare(scheduler, 1000, 1000, true, location(1), router) == Telemetry::DispatchResult::QUEUED);
    CHECK(scheduler.stop(peer(3), 1001) == Telemetry::ShareSessionResult::STOPPING);
    router.accepted = false;
    CHECK(Telemetry::dispatchLocationShare(scheduler, 1001, 1001, false, location(1), router) == Telemetry::DispatchResult::QUEUE_REJECTED);
    Telemetry::CustomLocationMeta meta{};
    Telemetry::BinaryView encoded_meta{};
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
              router.envelope.fields[1].value,
              router.envelope.fields[1].value_size,
              encoded_meta) == Telemetry::FieldValueResult::OK);
    CHECK(Telemetry::decodeCustomLocationMeta(
              encoded_meta.data, encoded_meta.size, meta) ==
          Telemetry::CustomMetaResult::OK);
    CHECK(meta.has_cease && meta.cease);
    Telemetry::BinaryView inner{};
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(router.envelope.fields[0].value, router.envelope.fields[0].value_size, inner) == Telemetry::FieldValueResult::OK);
    Telemetry::LocationTelemetry zero{};
    CHECK(Telemetry::decodeLocationTelemetry(inner.data, inner.size, zero) == Telemetry::DecodeResult::OK);
    CHECK(zero.latitude_e6 == 0 && zero.longitude_e6 == 0);
    CHECK(zero.timestamp_seconds == 1U &&
          zero.sensor_timestamp_seconds == 1U);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(3), session));
    router.accepted = true;
    CHECK(Telemetry::dispatchLocationShare(scheduler, session.next_attempt_millis, 2000, false, location(1), router) == Telemetry::DispatchResult::CEASE_QUEUED);
    CHECK(scheduler.size() == 0);
}

void optionalMetadataIsOmitted() {
    Telemetry::ShareWork work{};
    work.peer = peer(8);
    Telemetry::OutboundLocationEnvelope envelope{};
    CHECK(Telemetry::buildOutboundLocationEnvelope(
              work, location(1), 1000, envelope) ==
          Telemetry::EnvelopeBuildResult::OK);
    CHECK(envelope.field_count == 1);

    work.has_approx_radius = true;
    CHECK(Telemetry::buildOutboundLocationEnvelope(
              work, location(1), 1000, envelope) ==
          Telemetry::EnvelopeBuildResult::OK);
    CHECK(envelope.field_count == 2);
    Telemetry::BinaryView encoded_meta{};
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
              envelope.fields[1].value, envelope.fields[1].value_size,
              encoded_meta) == Telemetry::FieldValueResult::OK);
    Telemetry::CustomLocationMeta meta{};
    CHECK(Telemetry::decodeCustomLocationMeta(
              encoded_meta.data, encoded_meta.size, meta) ==
          Telemetry::CustomMetaResult::OK);
    CHECK(meta.has_approx_radius && meta.approx_radius_meters == 0);
}

void expiredOwnershipLeaseIsRejected() {
    Telemetry::LocationShareScheduler scheduler;
    Telemetry::ShareStartOptions options{};
    options.duration = Telemetry::ShareDuration::INDEFINITE;
    CHECK(scheduler.start(peer(12), options, 1000) ==
          Telemetry::ShareSessionResult::STARTED);
    FakeRouter router;
    router.ownership_time = 2000;
    CHECK(Telemetry::dispatchLocationShare(
              scheduler, 1000, 1000, true, location(1), router) ==
          Telemetry::DispatchResult::QUEUE_REJECTED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(12), session) && !session.has_sent &&
          !session.awaiting_ack);
}

void noSideEffectsOrWorkWithoutValidGps() {
    Telemetry::LocationShareScheduler scheduler;
    Telemetry::ShareStartOptions options{};
    options.duration = Telemetry::ShareDuration::INDEFINITE;
    CHECK(scheduler.start(peer(4), options, 1000) == Telemetry::ShareSessionResult::STARTED);
    FakeRouter router;
    CHECK(Telemetry::dispatchLocationShare(scheduler, 1000, 1000, false, location(1), router) == Telemetry::DispatchResult::NO_WORK);
    CHECK(router.calls == 0);
    CHECK(sizeof(Telemetry::OutboundLocationEnvelope) <= 768U);
}
}

int main() {
    exactLocationEnvelope();
    acceptanceAndRetryAreHonest();
    ceaseShapeAndOwnership();
    optionalMetadataIsOmitted();
    expiredOwnershipLeaseIsRejected();
    noSideEffectsOrWorkWithoutValidGps();
    std::cout << "location LXMF adapter: " << passed << " passed, " << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
