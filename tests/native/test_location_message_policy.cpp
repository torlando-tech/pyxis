#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "Telemetry/LocationMessagePolicy.h"

namespace {

int passed = 0;
int failures = 0;

#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

Telemetry::PeerId peer(uint8_t seed) {
    Telemetry::PeerId id{};
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) {
        id.bytes[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
    }
    return id;
}

struct Fields {
    uint8_t inner[128]{};
    uint8_t telemetry[132]{};
    uint8_t meta[128]{};
    std::size_t telemetry_size = 0;
    std::size_t meta_size = 0;
};

Fields validFields(uint64_t timestamp, bool with_meta = false, bool cease = false) {
    Fields fields{};
    Telemetry::LocationTelemetry location{};
    location.latitude_e6 = 12345678;
    location.longitude_e6 = -87654321;
    location.altitude_cm = 1234;
    location.speed_centi_kmh = 560;
    location.bearing_cdeg = 9000;
    location.accuracy_cm = 50;
    location.timestamp_seconds = timestamp;
    location.sensor_timestamp_seconds = timestamp + 1U;
    std::size_t inner_size = 0;
    CHECK(Telemetry::encodeLocationTelemetry(location, fields.inner, sizeof(fields.inner), inner_size) == Telemetry::EncodeResult::OK);
    CHECK(Telemetry::wrapLxmfBinaryFieldValue(fields.inner, inner_size, fields.telemetry, sizeof(fields.telemetry), fields.telemetry_size) == Telemetry::FieldValueResult::OK);
    if (with_meta) {
        Telemetry::CustomLocationMeta meta{};
        meta.has_cease = true;
        meta.cease = cease;
        meta.has_timestamp = true;
        meta.timestamp_millis = static_cast<int64_t>(timestamp * 1000U);
        CHECK(Telemetry::encodeCustomLocationMeta(meta, fields.meta, sizeof(fields.meta), fields.meta_size) == Telemetry::CustomMetaResult::OK);
    }
    return fields;
}

Telemetry::InboundLocationMessage input(const Telemetry::PeerId& sender, const Fields& fields) {
    Telemetry::InboundLocationMessage value{};
    value.authenticated_sender = sender;
    value.telemetry.present = true;
    value.telemetry.raw_value = Telemetry::BinaryView(fields.telemetry, fields.telemetry_size);
    value.received_at_millis = 50000;
    return value;
}

class FakeEffects : public Telemetry::LocationMessageEffects {
public:
    Telemetry::PeerLocationStore store{};
    char calls[256]{};
    std::size_t count = 0;
    bool persist_ok = true;
    Telemetry::PeerId applied_sender{};

    Telemetry::PeerLocationResult applyLocation(const Telemetry::PeerId& sender,
                                                 const Telemetry::LocationTelemetry& location,
                                                 const Telemetry::CustomLocationMeta& meta,
                                                 uint64_t received_at_millis) override {
        calls[count++] = 'L';
        applied_sender = sender;
        return store.apply(sender, location, meta, received_at_millis);
    }
    bool persistMessage() override { calls[count++] = 'P'; return persist_ok; }
    void routeMessage() override { calls[count++] = 'R'; }
    void notifyMessage() override { calls[count++] = 'N'; }
    void logMalformedLocation() override { calls[count++] = 'E'; }
};

bool samePeer(const Telemetry::PeerId& a, const Telemetry::PeerId& b) {
    return std::memcmp(a.bytes, b.bytes, Telemetry::PEER_ID_SIZE) == 0;
}

void validTelemetryPolicyMatrixAndOrdering() {
    const Telemetry::PeerId sender = peer(1);
    Fields fields = validFields(10);
    Telemetry::InboundLocationMessage message = input(sender, fields);

    Telemetry::LocationMessageDecision decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::VALID_LOCATION);
    CHECK(decision.apply_location && !decision.persist && !decision.route && !decision.notify);
    CHECK(!decision.drop && !decision.log_malformed);
    CHECK(samePeer(decision.authenticated_sender, sender));

    FakeEffects effects;
    Telemetry::LocationMessageExecution result = Telemetry::executeLocationMessageDecision(decision, effects);
    CHECK(result.location_result == Telemetry::PeerLocationResult::INSERTED);
    CHECK(effects.count == 1 && effects.calls[0] == 'L');

    message.title = Telemetry::TextField(true, nullptr, 0); // present but empty
    message.content = Telemetry::TextField(true, reinterpret_cast<const uint8_t*>("hello"), 5);
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.apply_location && decision.persist && decision.route && decision.notify);
    FakeEffects mixed;
    result = Telemetry::executeLocationMessageDecision(decision, mixed);
    CHECK(result.persisted);
    CHECK(mixed.count == 4 && std::memcmp(mixed.calls, "LPRN", 4) == 0);

    message.content = Telemetry::TextField(false, nullptr, 0);
    message.title = Telemetry::TextField(true, reinterpret_cast<const uint8_t*>("title"), 5);
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.apply_location && decision.persist && decision.route && decision.notify);
}

void absenceEmptyAndNonLocationCases() {
    Telemetry::InboundLocationMessage message{};
    message.authenticated_sender = peer(2);
    Telemetry::LocationMessageDecision decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::NOT_LOCATION);
    CHECK(!decision.apply_location && decision.persist && decision.route && decision.notify);

    message.title = Telemetry::TextField(true, nullptr, 0);
    message.content = Telemetry::TextField(true, nullptr, 0);
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::NOT_LOCATION);
    CHECK(decision.persist && decision.notify);

    const uint8_t malformed_meta[] = {0x81U};
    message.custom_meta.present = true;
    message.custom_meta.raw_value = Telemetry::BinaryView(malformed_meta, sizeof(malformed_meta));
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::NOT_LOCATION);
    CHECK(decision.persist && !decision.log_malformed);
}

void malformedLocationFailsClosedButTextSurvives() {
    Telemetry::InboundLocationMessage empty_field{};
    empty_field.authenticated_sender = peer(3);
    empty_field.telemetry.present = true;
    Telemetry::LocationMessageDecision empty_decision =
        Telemetry::classifyInboundLocationMessage(empty_field);
    CHECK(empty_decision.kind == Telemetry::LocationMessageKind::MALFORMED_LOCATION);
    CHECK(empty_decision.drop && empty_decision.log_malformed);

    const uint8_t non_bin[] = {0x80U};
    Telemetry::InboundLocationMessage message{};
    message.authenticated_sender = peer(3);
    message.telemetry.present = true;
    message.telemetry.raw_value = Telemetry::BinaryView(non_bin, sizeof(non_bin));
    Telemetry::LocationMessageDecision decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::MALFORMED_LOCATION);
    CHECK(!decision.apply_location && !decision.persist && !decision.notify);
    CHECK(decision.drop && decision.log_malformed);
    FakeEffects empty;
    Telemetry::executeLocationMessageDecision(decision, empty);
    CHECK(empty.count == 1 && empty.calls[0] == 'E');

    message.content = Telemetry::TextField(true, reinterpret_cast<const uint8_t*>("x"), 1);
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(!decision.apply_location && decision.persist && decision.route && decision.notify);
    CHECK(!decision.drop && decision.log_malformed);
    FakeEffects text;
    Telemetry::executeLocationMessageDecision(decision, text);
    CHECK(text.count == 4 && std::memcmp(text.calls, "EPRN", 4) == 0);

    Fields fields = validFields(12);
    message = input(peer(3), fields);
    const uint8_t malformed_meta[] = {0x81U, 0xa5U, 'c', 'e', 'a', 's', 'e'};
    message.custom_meta.present = true;
    message.custom_meta.raw_value = Telemetry::BinaryView(malformed_meta, sizeof(malformed_meta));
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::MALFORMED_LOCATION);
    CHECK(!decision.apply_location && decision.drop && decision.log_malformed);

    message.telemetry.raw_value = Telemetry::BinaryView(nullptr, 1);
    decision = Telemetry::classifyInboundLocationMessage(message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::MALFORMED_LOCATION);
    CHECK(!decision.apply_location);
}

void ceaseAndAuthenticatedSenderIsolation() {
    const Telemetry::PeerId first = peer(10);
    const Telemetry::PeerId second = peer(40);
    Fields ordinary = validFields(20);
    FakeEffects effects;
    Telemetry::executeLocationMessageDecision(Telemetry::classifyInboundLocationMessage(input(first, ordinary)), effects);
    Telemetry::executeLocationMessageDecision(Telemetry::classifyInboundLocationMessage(input(second, ordinary)), effects);
    CHECK(effects.store.size() == 2);

    Fields cease = validFields(20, true, true);
    Telemetry::InboundLocationMessage cease_message = input(first, cease);
    cease_message.custom_meta.present = true;
    cease_message.custom_meta.raw_value = Telemetry::BinaryView(cease.meta, cease.meta_size);
    Telemetry::LocationMessageDecision decision = Telemetry::classifyInboundLocationMessage(cease_message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::VALID_CEASE);
    CHECK(decision.apply_location && !decision.persist && !decision.notify);
    Telemetry::executeLocationMessageDecision(decision, effects);
    Telemetry::PeerLocationRecord record{};
    CHECK(!effects.store.get(first, record));
    CHECK(effects.store.get(second, record));
    CHECK(samePeer(effects.applied_sender, first));

    Fields cease_false = validFields(21, true, false);
    Telemetry::InboundLocationMessage false_message = input(first, cease_false);
    false_message.custom_meta.present = true;
    false_message.custom_meta.raw_value =
        Telemetry::BinaryView(cease_false.meta, cease_false.meta_size);
    decision = Telemetry::classifyInboundLocationMessage(false_message);
    CHECK(decision.kind == Telemetry::LocationMessageKind::VALID_LOCATION);
    CHECK(decision.apply_location);

    cease_message.content = Telemetry::TextField(true, reinterpret_cast<const uint8_t*>("stopped"), 7);
    decision = Telemetry::classifyInboundLocationMessage(cease_message);
    CHECK(decision.apply_location && decision.persist && decision.route && decision.notify);
}

void staleDuplicateCapacityAndPersistenceFailure() {
    FakeEffects effects;
    for (std::size_t i = 0; i < Telemetry::MAX_PEER_LOCATIONS + 1U; ++i) {
        Fields fields = validFields(100U + static_cast<uint64_t>(i));
        Telemetry::InboundLocationMessage message = input(peer(static_cast<uint8_t>(i)), fields);
        message.received_at_millis = static_cast<uint64_t>(i);
        Telemetry::executeLocationMessageDecision(Telemetry::classifyInboundLocationMessage(message), effects);
    }
    CHECK(effects.store.size() == Telemetry::MAX_PEER_LOCATIONS);
    Telemetry::PeerLocationRecord record{};
    CHECK(!effects.store.get(peer(0), record));
    CHECK(effects.store.get(peer(static_cast<uint8_t>(Telemetry::MAX_PEER_LOCATIONS)), record));

    Fields newer = validFields(500);
    Telemetry::InboundLocationMessage message = input(peer(99), newer);
    Telemetry::executeLocationMessageDecision(Telemetry::classifyInboundLocationMessage(message), effects);
    Fields stale = validFields(499);
    message = input(peer(99), stale);
    Telemetry::LocationMessageExecution execution = Telemetry::executeLocationMessageDecision(Telemetry::classifyInboundLocationMessage(message), effects);
    CHECK(execution.location_result == Telemetry::PeerLocationResult::STALE);

    Fields fields = validFields(700);
    message = input(peer(100), fields);
    message.content = Telemetry::TextField(true, reinterpret_cast<const uint8_t*>("chat"), 4);
    FakeEffects failed;
    failed.persist_ok = false;
    execution = Telemetry::executeLocationMessageDecision(Telemetry::classifyInboundLocationMessage(message), failed);
    CHECK(!execution.persisted);
    CHECK(failed.count == 2 && std::memcmp(failed.calls, "LP", 2) == 0);
}

void sanitizerStressAndBounds() {
    CHECK(sizeof(Telemetry::LocationMessageDecision) <= 160U);
    CHECK(sizeof(Telemetry::InboundLocationMessage) <= 128U);
    uint8_t fuzz[32]{};
    for (std::size_t i = 0; i < 100000U; ++i) {
        for (std::size_t j = 0; j < sizeof(fuzz); ++j) fuzz[j] = static_cast<uint8_t>(i + j * 17U);
        Telemetry::InboundLocationMessage message{};
        message.authenticated_sender = peer(static_cast<uint8_t>(i));
        message.telemetry.present = true;
        message.telemetry.raw_value = Telemetry::BinaryView(fuzz, i % sizeof(fuzz));
        if ((i & 1U) != 0U) message.content = Telemetry::TextField(true, fuzz, 1);
        const Telemetry::LocationMessageDecision decision = Telemetry::classifyInboundLocationMessage(message);
        CHECK(!decision.apply_location || decision.kind == Telemetry::LocationMessageKind::VALID_LOCATION || decision.kind == Telemetry::LocationMessageKind::VALID_CEASE);
    }
}

}  // namespace

int main() {
    validTelemetryPolicyMatrixAndOrdering();
    absenceEmptyAndNonLocationCases();
    malformedLocationFailsClosedButTextSurvives();
    ceaseAndAuthenticatedSenderIsolation();
    staleDuplicateCapacityAndPersistenceFailure();
    sanitizerStressAndBounds();
    std::cout << "location message policy: " << passed << " passed, " << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
