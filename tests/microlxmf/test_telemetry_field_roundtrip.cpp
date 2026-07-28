#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include <LXMF/LXMessage.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Identity.h>

int main() {
    constexpr uint8_t key_bytes[] = {0x02};
    constexpr uint8_t inner_telemeter[] = {
        0x82, 0x01, 0xce, 0x65, 0x53, 0xf1, 0x00, 0x02, 0x97,
        0xc4, 0x04, 0x02, 0x40, 0x66, 0x34,
        0xc4, 0x04, 0xf8, 0xb4, 0x07, 0x38,
        0xc4, 0x04, 0x00, 0x00, 0x06, 0x40,
        0xc4, 0x04, 0x00, 0x00, 0x04, 0xd2,
        0xc4, 0x04, 0x00, 0x00, 0x10, 0x68,
        0xc4, 0x02, 0x01, 0x5e,
        0xce, 0x65, 0x53, 0xf1, 0x00,
    };
    uint8_t raw_value[sizeof(inner_telemeter) + 2]{};
    raw_value[0] = 0xc4;
    raw_value[1] = static_cast<uint8_t>(sizeof(inner_telemeter));
    for (std::size_t index = 0; index < sizeof(inner_telemeter); ++index) {
        raw_value[index + 2] = inner_telemeter[index];
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
    const RNS::Bytes value(raw_value, sizeof(raw_value));
    if (!message.fields_set(key, value)) {
        std::cerr << "fields_set rejected telemetry\n";
        return EXIT_FAILURE;
    }

    const RNS::Bytes packed = message.pack();
    LXMF::LXMessage decoded = LXMF::LXMessage::unpack_from_bytes(
        packed, LXMF::Type::Message::OPPORTUNISTIC, true);
    const RNS::Bytes* decoded_value = decoded.fields_get(key);
    if (decoded_value == nullptr || *decoded_value != value) {
        std::cerr << "raw telemetry BIN span changed across microLXMF pack/unpack\n";
        return EXIT_FAILURE;
    }

    std::cout << "microLXMF telemetry field roundtrip: passed\n";
    return EXIT_SUCCESS;
}
