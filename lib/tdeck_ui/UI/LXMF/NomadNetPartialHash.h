#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace UI::LXMF::NomadNet {
namespace PartialHashDetail {

constexpr std::array<uint32_t, 64> ROUND_CONSTANTS{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

inline uint32_t rotate_right(uint32_t value, uint8_t bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

inline void transform(const uint8_t block[64],
                      std::array<uint32_t, 8>& state) noexcept {
    uint32_t words[64]{};
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotate_right(words[i - 15], 7) ^
            rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const uint32_t s1 = rotate_right(words[i - 2], 17) ^
            rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
            rotate_right(e, 25);
        const uint32_t choice = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + choice + ROUND_CONSTANTS[i] + words[i];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
            rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace PartialHashDetail

// Standalone so native parser harnesses and ESP32 builds use identical bytes
// without depending on a platform-specific crypto provider.
inline std::array<uint8_t, 32> partial_descriptor_sha256(
        const char* data, std::size_t size) noexcept {
    using namespace PartialHashDetail;
    std::array<uint32_t, 8> state{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    std::size_t offset = 0;
    while (size - offset >= 64) {
        transform(reinterpret_cast<const uint8_t*>(data + offset), state);
        offset += 64;
    }

    uint8_t final_blocks[128]{};
    const std::size_t remainder = size - offset;
    if (remainder != 0) std::memcpy(final_blocks, data + offset, remainder);
    final_blocks[remainder] = 0x80;
    const std::size_t final_size = remainder < 56 ? 64 : 128;
    const uint64_t bit_length = static_cast<uint64_t>(size) * 8U;
    for (std::size_t i = 0; i < 8; ++i)
        final_blocks[final_size - 1 - i] = static_cast<uint8_t>(bit_length >> (i * 8));
    transform(final_blocks, state);
    if (final_size == 128) transform(final_blocks + 64, state);

    std::array<uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        digest[i * 4] = static_cast<uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    return digest;
}

} // namespace UI::LXMF::NomadNet
