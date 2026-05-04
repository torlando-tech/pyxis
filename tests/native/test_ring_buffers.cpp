// Native unit tests for PCM and encoded SPSC ring buffers.
//
// LXST tuning numbers (PCM_RING_FRAMES=50, PREBUFFER_FRAMES=15,
// ENCODED_RING_SLOTS=128) live in code that consumes these buffers, but
// correctness of overflow/underrun/wraparound behavior is what makes those
// numbers meaningful. Tests:
//
//   PacketRingBuffer (fixed frame size):
//     - empty read returns false
//     - write+read preserves samples bit-exact
//     - wrong-size write/read rejected
//     - capacity is maxFrames - 1 (one slot reserved as the empty/full flag)
//     - full ring rejects writes; partial drain frees a slot
//     - wraparound across the buffer end preserves data
//     - availableFrames consistent across operations
//     - reset clears producer + consumer
//     - SPSC stress: producer thread + consumer thread, no loss/reorder
//
//   EncodedRingBuffer (variable-length slots):
//     - length=0 rejected
//     - length > maxBytesPerSlot rejected
//     - round-trip preserves payload + length
//     - read with too-small dest advances read cursor and returns false
//     - wraparound

#include "../../lib/lxst_audio/packet_ring_buffer.h"
#include "../../lib/lxst_audio/encoded_ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

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

// ── PacketRingBuffer tests ──

static std::vector<int16_t> make_frame(int n, int seed) {
    std::vector<int16_t> f(n);
    for (int i = 0; i < n; ++i) f[i] = (int16_t)((i * 31) ^ seed);
    return f;
}

static void prb_empty_read_returns_false() {
    PacketRingBuffer rb(4, 8);
    int16_t out[8];
    EXPECT_TRUE(!rb.read(out, 8));
    EXPECT_EQ(rb.availableFrames(), 0);
}

static void prb_write_read_round_trip() {
    PacketRingBuffer rb(4, 8);
    auto f = make_frame(8, 0xAA);
    EXPECT_TRUE(rb.write(f.data(), 8));
    EXPECT_EQ(rb.availableFrames(), 1);

    int16_t out[8] = {0};
    EXPECT_TRUE(rb.read(out, 8));
    for (int i = 0; i < 8; ++i) EXPECT_EQ(out[i], f[i]);
    EXPECT_EQ(rb.availableFrames(), 0);
}

static void prb_wrong_size_rejected() {
    PacketRingBuffer rb(4, 8);
    auto f = make_frame(8, 0x55);
    int16_t out[8];
    EXPECT_TRUE(!rb.write(f.data(), 7));   // wrong count
    EXPECT_TRUE(!rb.write(f.data(), 9));
    rb.write(f.data(), 8);
    EXPECT_TRUE(!rb.read(out, 7));
    EXPECT_TRUE(!rb.read(out, 9));
}

static void prb_capacity_is_max_minus_one() {
    // SPSC with one-slot-reserved-for-empty: capacity is maxFrames - 1.
    const int N = 4;
    PacketRingBuffer rb(N, 4);
    auto f = make_frame(4, 0x11);
    int wrote = 0;
    while (rb.write(f.data(), 4)) ++wrote;
    EXPECT_EQ(wrote, N - 1);              // 3 writes succeed before full
    EXPECT_EQ(rb.availableFrames(), N - 1);
}

static void prb_partial_drain_frees_slots() {
    PacketRingBuffer rb(4, 4);
    auto f = make_frame(4, 0x22);
    while (rb.write(f.data(), 4)) {}
    EXPECT_EQ(rb.availableFrames(), 3);

    int16_t out[4];
    EXPECT_TRUE(rb.read(out, 4));
    EXPECT_EQ(rb.availableFrames(), 2);
    EXPECT_TRUE(rb.write(f.data(), 4));   // slot freed up
    EXPECT_EQ(rb.availableFrames(), 3);
}

static void prb_wraparound_preserves_data() {
    PacketRingBuffer rb(4, 4);
    int16_t out[4];
    // Cycle 10 distinct frames through; if wraparound is broken, data
    // corrupts at the buffer-end boundary.
    for (int i = 0; i < 10; ++i) {
        auto f = make_frame(4, 0x40 + i);
        EXPECT_TRUE(rb.write(f.data(), 4));
        EXPECT_TRUE(rb.read(out, 4));
        for (int j = 0; j < 4; ++j) EXPECT_EQ(out[j], f[j]);
    }
}

static void prb_reset_clears_state() {
    PacketRingBuffer rb(4, 4);
    auto f = make_frame(4, 0x33);
    rb.write(f.data(), 4);
    rb.write(f.data(), 4);
    EXPECT_EQ(rb.availableFrames(), 2);
    rb.reset();
    EXPECT_EQ(rb.availableFrames(), 0);
}

// SPSC stress: separate producer and consumer threads exchange a million
// frames. Verifies acquire/release ordering on writeIndex_/readIndex_ keeps
// data intact (no torn frame, no reordering, no loss when sized big enough).
static void prb_spsc_threaded_stress() {
    const int FRAMES = 8;
    PacketRingBuffer rb(64, FRAMES);
    const int TOTAL = 100000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]{
        for (int i = 0; i < TOTAL; ++i) {
            std::vector<int16_t> f(FRAMES, (int16_t)(i & 0x7FFF));
            while (!rb.write(f.data(), FRAMES)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    int received = 0;
    int failures = 0;
    while (received < TOTAL) {
        int16_t out[FRAMES];
        if (rb.read(out, FRAMES)) {
            int16_t expected = (int16_t)(received & 0x7FFF);
            for (int j = 0; j < FRAMES; ++j) {
                if (out[j] != expected) ++failures;
            }
            ++received;
        } else {
            std::this_thread::yield();
            if (producer_done.load(std::memory_order_acquire) &&
                rb.availableFrames() == 0) break;
        }
    }
    producer.join();

    EXPECT_EQ(received, TOTAL);
    EXPECT_EQ(failures, 0);
}

// ── EncodedRingBuffer tests ──

static void erb_zero_length_rejected() {
    EncodedRingBuffer eb(4, 64);
    uint8_t data[1] = {0xAA};
    EXPECT_TRUE(!eb.write(data, 0));
    EXPECT_TRUE(!eb.write(data, -1));
}

static void erb_oversize_rejected() {
    EncodedRingBuffer eb(4, 16);
    uint8_t big[32]; std::memset(big, 0xCC, sizeof(big));
    EXPECT_TRUE(!eb.write(big, 17));
    EXPECT_TRUE(eb.write(big, 16));
}

static void erb_round_trip_variable_lengths() {
    EncodedRingBuffer eb(8, 64);
    std::vector<std::vector<uint8_t>> sent;

    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> v;
        int len = 1 + i * 7;
        for (int j = 0; j < len; ++j) v.push_back((uint8_t)((j ^ i) * 31));
        sent.push_back(v);
        EXPECT_TRUE(eb.write(v.data(), len));
    }
    EXPECT_EQ(eb.availableSlots(), 5);

    for (size_t i = 0; i < sent.size(); ++i) {
        uint8_t out[128] = {0};
        int actual = -1;
        EXPECT_TRUE(eb.read(out, 128, &actual));
        EXPECT_EQ(actual, (int)sent[i].size());
        for (int j = 0; j < actual; ++j) EXPECT_EQ(out[j], sent[i][j]);
    }
    EXPECT_EQ(eb.availableSlots(), 0);
}

static void erb_too_small_dest_advances_cursor() {
    // Per impl: if maxLength < stored length, read advances readIndex and
    // returns false with actualLength=0. This is intentional drop-on-truncation.
    EncodedRingBuffer eb(4, 64);
    uint8_t big[40]; std::memset(big, 0x88, sizeof(big));
    eb.write(big, 40);
    EXPECT_EQ(eb.availableSlots(), 1);

    uint8_t small[10];
    int actual = -1;
    EXPECT_TRUE(!eb.read(small, 10, &actual));
    EXPECT_EQ(actual, 0);
    EXPECT_EQ(eb.availableSlots(), 0);   // cursor advanced, slot consumed
}

static void erb_wraparound() {
    EncodedRingBuffer eb(4, 8);
    uint8_t buf[8];
    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 8; ++j) buf[j] = (uint8_t)((i + j) * 13);
        EXPECT_TRUE(eb.write(buf, 8));
        uint8_t out[8] = {0};
        int actual = -1;
        EXPECT_TRUE(eb.read(out, 8, &actual));
        EXPECT_EQ(actual, 8);
        for (int j = 0; j < 8; ++j) EXPECT_EQ(out[j], buf[j]);
    }
}

static void erb_full_then_drain() {
    EncodedRingBuffer eb(4, 8);
    uint8_t data[4] = {1,2,3,4};
    int wrote = 0;
    while (eb.write(data, 4)) ++wrote;
    EXPECT_EQ(wrote, 3);                  // capacity = N - 1

    int actual;
    uint8_t out[8];
    while (eb.read(out, 8, &actual)) {}
    EXPECT_EQ(eb.availableSlots(), 0);
}

int main() {
    RUN(prb_empty_read_returns_false);
    RUN(prb_write_read_round_trip);
    RUN(prb_wrong_size_rejected);
    RUN(prb_capacity_is_max_minus_one);
    RUN(prb_partial_drain_frees_slots);
    RUN(prb_wraparound_preserves_data);
    RUN(prb_reset_clears_state);
    RUN(prb_spsc_threaded_stress);

    RUN(erb_zero_length_rejected);
    RUN(erb_oversize_rejected);
    RUN(erb_round_trip_variable_lengths);
    RUN(erb_too_small_dest_advances_cursor);
    RUN(erb_wraparound);
    RUN(erb_full_then_drain);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
