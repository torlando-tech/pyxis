// Native HDLC unit tests.
//
// Verifies pyxis's HDLC framing matches the Python RNS TCPInterface spec:
//   FLAG     = 0x7E
//   ESC      = 0x7D
//   ESC_MASK = 0x20
//   In payload: 0x7E -> 0x7D 0x5E   (ESC + (FLAG ^ ESC_MASK))
//               0x7D -> 0x7D 0x5D   (ESC + (ESC  ^ ESC_MASK))
//
// Build: see test_hdlc.py for the g++ invocation.

#include "../../src/HDLC.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using RNS::HDLC;
using RNS::Bytes;

// ── minimal test framework ──

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_EQ(actual, expected)                                            \
    do {                                                                       \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (!(_a == _e)) {                                                     \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: %s != %s",                 \
                          __FILE__, __LINE__, #actual, #expected);             \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define EXPECT_TRUE(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            char buf[256];                                                     \
            std::snprintf(buf, sizeof(buf), "%s:%d: expected %s",              \
                          __FILE__, __LINE__, #cond);                          \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)

#define RUN(name)                                                              \
    do {                                                                       \
        try {                                                                  \
            name();                                                            \
            ++g_pass;                                                          \
            std::printf("PASS %s\n", #name);                                   \
        } catch (const std::exception& e) {                                    \
            ++g_fail;                                                          \
            std::printf("FAIL %s: %s\n", #name, e.what());                     \
        }                                                                      \
    } while (0)

static Bytes make_bytes(std::initializer_list<uint8_t> bs) {
    Bytes b;
    for (uint8_t x : bs) b.append(x);
    return b;
}

static std::string hex(const Bytes& b) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (size_t i = 0; i < b.size(); ++i) {
        s.push_back(digits[b.data()[i] >> 4]);
        s.push_back(digits[b.data()[i] & 0xF]);
    }
    return s;
}

// ── tests ──

static void empty_payload_escape() {
    Bytes empty;
    EXPECT_EQ(HDLC::escape(empty).size(), (size_t)0);
    EXPECT_EQ(HDLC::unescape(empty).size(), (size_t)0);
    Bytes framed = HDLC::frame(empty);
    EXPECT_EQ(framed.size(), (size_t)2);
    EXPECT_EQ(framed.data()[0], HDLC::FLAG);
    EXPECT_EQ(framed.data()[1], HDLC::FLAG);
}

static void plain_payload_passes_through() {
    Bytes data = make_bytes({0x01, 0x02, 0x03, 0x04});
    Bytes esc = HDLC::escape(data);
    EXPECT_EQ(esc, data);
    Bytes round = HDLC::unescape(esc);
    EXPECT_EQ(round, data);
}

static void escape_single_flag() {
    Bytes data = make_bytes({0x7E});
    Bytes esc = HDLC::escape(data);
    EXPECT_EQ(esc.size(), (size_t)2);
    EXPECT_EQ(esc.data()[0], HDLC::ESC);
    EXPECT_EQ(esc.data()[1], (uint8_t)(HDLC::FLAG ^ HDLC::ESC_MASK));
    EXPECT_EQ(hex(esc), std::string("7d5e"));
}

static void escape_single_esc() {
    Bytes data = make_bytes({0x7D});
    Bytes esc = HDLC::escape(data);
    EXPECT_EQ(esc.size(), (size_t)2);
    EXPECT_EQ(esc.data()[0], HDLC::ESC);
    EXPECT_EQ(esc.data()[1], (uint8_t)(HDLC::ESC ^ HDLC::ESC_MASK));
    EXPECT_EQ(hex(esc), std::string("7d5d"));
}

static void round_trip_all_byte_values() {
    Bytes data;
    for (int i = 0; i < 256; ++i) data.append((uint8_t)i);
    Bytes esc = HDLC::escape(data);
    Bytes round = HDLC::unescape(esc);
    EXPECT_EQ(round, data);

    // Escaped output should contain no bare FLAG byte. (Receiver relies on
    // this — FLAG only appears at frame boundaries, never inside the payload.)
    for (size_t i = 0; i < esc.size(); ++i) {
        EXPECT_TRUE(esc.data()[i] != HDLC::FLAG);
    }
}

static void frame_round_trip_simple() {
    Bytes payload = make_bytes({0x01, 0x7E, 0x02, 0x7D, 0x03});
    Bytes framed = HDLC::frame(payload);
    EXPECT_EQ(framed.data()[0], HDLC::FLAG);
    EXPECT_EQ(framed.data()[framed.size() - 1], HDLC::FLAG);

    Bytes inner;
    for (size_t i = 1; i < framed.size() - 1; ++i) inner.append(framed.data()[i]);
    Bytes recovered = HDLC::unescape(inner);
    EXPECT_EQ(recovered, payload);
}

static void unescape_truncated_returns_empty() {
    // ESC at end with no follow byte is an error per HDLC.h; returns empty.
    Bytes broken = make_bytes({0x01, 0x02, HDLC::ESC});
    Bytes result = HDLC::unescape(broken);
    EXPECT_EQ(result.size(), (size_t)0);
}

static void unescape_non_canonical_escape_byte() {
    // HDLC.h XORs anything-after-ESC with ESC_MASK regardless of canonical
    // value. Document the behavior — Python RNS does the same.
    Bytes broken = make_bytes({HDLC::ESC, 0x00});
    Bytes result = HDLC::unescape(broken);
    EXPECT_EQ(result.size(), (size_t)1);
    EXPECT_EQ(result.data()[0], (uint8_t)0x20);
}

static void round_trip_long_payload_with_many_escapes() {
    Bytes data;
    for (int i = 0; i < 1024; ++i) data.append((uint8_t)((i * 31) ^ 0x55));
    Bytes esc = HDLC::escape(data);
    Bytes round = HDLC::unescape(esc);
    EXPECT_EQ(round, data);
}

static void escape_worst_case_doubles_size() {
    Bytes data;
    for (int i = 0; i < 100; ++i) data.append(HDLC::FLAG);
    Bytes esc = HDLC::escape(data);
    EXPECT_EQ(esc.size(), data.size() * 2);
}

static void golden_vector_matches_python_rns() {
    // Computed against the spec: payload {0x01, 0x7E, 0x7D, 0x02} →
    //   0x01, 0x7D, 0x5E, 0x7D, 0x5D, 0x02
    // Framed: 0x7E ... 0x7E
    Bytes payload = make_bytes({0x01, 0x7E, 0x7D, 0x02});
    Bytes esc = HDLC::escape(payload);
    EXPECT_EQ(hex(esc), std::string("017d5e7d5d02"));
    Bytes framed = HDLC::frame(payload);
    EXPECT_EQ(hex(framed), std::string("7e017d5e7d5d027e"));
}

int main() {
    RUN(empty_payload_escape);
    RUN(plain_payload_passes_through);
    RUN(escape_single_flag);
    RUN(escape_single_esc);
    RUN(round_trip_all_byte_values);
    RUN(frame_round_trip_simple);
    RUN(unescape_truncated_returns_empty);
    RUN(unescape_non_canonical_escape_byte);
    RUN(round_trip_long_payload_with_many_escapes);
    RUN(escape_worst_case_doubles_size);
    RUN(golden_vector_matches_python_rns);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
