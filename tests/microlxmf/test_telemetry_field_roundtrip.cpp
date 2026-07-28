#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include <LXMF/LXMessage.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Identity.h>

#include "Telemetry/LocationTelemetryCodec.h"

int main() {
    constexpr uint8_t key_bytes[] = {Telemetry::FIELD_TELEMETRY};
    constexpr std::size_t TELEMETRY_CAPACITY = 128;

    Telemetry::LocationTelemetry outbound{};
    outbound.latitude_e6 = 37774900;
    outbound.longitude_e6 = -122419400;
    outbound.altitude_cm = 1600;
    outbound.speed_centi_kmh = 1234;
    outbound.bearing_cdeg = 4200;
    outbound.accuracy_cm = 350;
    outbound.timestamp_seconds = 1700000000ULL;
    outbound.sensor_timestamp_seconds = 1700000000ULL;

    uint8_t inner_telemeter[TELEMETRY_CAPACITY]{};
    std::size_t inner_size = 0;
    if (Telemetry::encodeLocationTelemetry(
            outbound, inner_telemeter, sizeof(inner_telemeter), inner_size) !=
        Telemetry::EncodeResult::OK) {
        std::cerr << "codec failed to encode telemetry\n";
        return EXIT_FAILURE;
    }

    uint8_t raw_value[TELEMETRY_CAPACITY + 5]{};
    std::size_t raw_size = 0;
    if (Telemetry::wrapLxmfBinaryFieldValue(
            inner_telemeter, inner_size, raw_value, sizeof(raw_value), raw_size) !=
        Telemetry::FieldValueResult::OK) {
        std::cerr << "codec failed to wrap telemetry field\n";
        return EXIT_FAILURE;
    }

    RNS::Identity source_identity;
    RNS::Identity destination_identity;
    RNS::Destination source(
        source_identity, RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    RNS::Destination destination(
        destination_identity, RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE, "lxmf", "delivery");

    LXMF::LXMessage message(
        destination, source, RNS::Bytes{}, RNS::Bytes{},
        LXMF::Type::Message::OPPORTUNISTIC);
    const RNS::Bytes key(key_bytes, sizeof(key_bytes));
    const RNS::Bytes value(raw_value, raw_size);
    if (!message.fields_set(key, value)) {
        std::cerr << "fields_set rejected telemetry\n";
        return EXIT_FAILURE;
    }

    const RNS::Bytes packed = message.pack();
    LXMF::LXMessage decoded_message = LXMF::LXMessage::unpack_from_bytes(
        packed, LXMF::Type::Message::OPPORTUNISTIC, true);
    const RNS::Bytes* decoded_value = decoded_message.fields_get(key);
    if (decoded_value == nullptr || *decoded_value != value) {
        std::cerr << "raw telemetry BIN span changed across microLXMF pack/unpack\n";
        return EXIT_FAILURE;
    }

    Telemetry::BinaryView unwrapped{};
    if (Telemetry::unwrapLxmfBinaryFieldValue(
            decoded_value->data(), decoded_value->size(), unwrapped) !=
        Telemetry::FieldValueResult::OK) {
        std::cerr << "codec failed to unwrap microLXMF field\n";
        return EXIT_FAILURE;
    }
    Telemetry::LocationTelemetry inbound{};
    if (Telemetry::decodeLocationTelemetry(
            unwrapped.data, unwrapped.size, inbound) !=
            Telemetry::DecodeResult::OK ||
        inbound.latitude_e6 != outbound.latitude_e6 ||
        inbound.longitude_e6 != outbound.longitude_e6 ||
        inbound.speed_centi_kmh != outbound.speed_centi_kmh ||
        inbound.timestamp_seconds != outbound.timestamp_seconds ||
        inbound.sensor_timestamp_seconds != outbound.sensor_timestamp_seconds) {
        std::cerr << "telemetry changed across codec/microLXMF roundtrip\n";
        return EXIT_FAILURE;
    }

    std::cout << "microLXMF telemetry field roundtrip: passed\n";
    return EXIT_SUCCESS;
}
