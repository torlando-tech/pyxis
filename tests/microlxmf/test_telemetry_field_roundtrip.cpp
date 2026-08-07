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
    constexpr uint8_t meta_key_bytes[] = {
        0xccU, Telemetry::FIELD_CUSTOM_META};
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

    Telemetry::CustomLocationMeta outbound_meta{};
    outbound_meta.has_expires = true;
    outbound_meta.expires_millis = 1700000900000LL;
    outbound_meta.has_approx_radius = true;
    outbound_meta.approx_radius_meters = 0;
    uint8_t inner_meta[96]{};
    std::size_t inner_meta_size = 0;
    if (Telemetry::encodeCustomLocationMeta(
            outbound_meta, inner_meta, sizeof(inner_meta), inner_meta_size) !=
        Telemetry::CustomMetaResult::OK) {
        std::cerr << "codec failed to encode custom metadata\n";
        return EXIT_FAILURE;
    }
    uint8_t raw_meta[101]{};
    std::size_t raw_meta_size = 0;
    if (Telemetry::wrapLxmfBinaryFieldValue(
            inner_meta, inner_meta_size, raw_meta, sizeof(raw_meta),
            raw_meta_size) != Telemetry::FieldValueResult::OK) {
        std::cerr << "codec failed to wrap custom metadata\n";
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
    const RNS::Bytes meta_key(meta_key_bytes, sizeof(meta_key_bytes));
    const RNS::Bytes meta_value(raw_meta, raw_meta_size);
    if (!message.fields_set(key, value) ||
        !message.fields_set(meta_key, meta_value)) {
        std::cerr << "fields_set rejected location fields\n";
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
    const RNS::Bytes* decoded_meta_value = decoded_message.fields_get(meta_key);
    if (decoded_meta_value == nullptr || *decoded_meta_value != meta_value) {
        std::cerr << "raw metadata BIN span changed across microLXMF pack/unpack\n";
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

    Telemetry::BinaryView unwrapped_meta{};
    if (Telemetry::unwrapLxmfBinaryFieldValue(
            decoded_meta_value->data(), decoded_meta_value->size(),
            unwrapped_meta) != Telemetry::FieldValueResult::OK) {
        std::cerr << "codec failed to unwrap microLXMF metadata field\n";
        return EXIT_FAILURE;
    }
    Telemetry::CustomLocationMeta inbound_meta{};
    if (Telemetry::decodeCustomLocationMeta(
            unwrapped_meta.data, unwrapped_meta.size, inbound_meta) !=
            Telemetry::CustomMetaResult::OK ||
        !inbound_meta.has_expires ||
        inbound_meta.expires_millis != outbound_meta.expires_millis ||
        !inbound_meta.has_approx_radius ||
        inbound_meta.approx_radius_meters != 0) {
        std::cerr << "custom metadata changed across codec/microLXMF roundtrip\n";
        return EXIT_FAILURE;
    }

    std::cout << "microLXMF location fields roundtrip: passed\n";
    return EXIT_SUCCESS;
}
