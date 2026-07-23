#include "../../lib/tdeck_ui/UI/LXMF/CallStartMailbox.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

using UI::LXMF::CallStartMailbox;

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

static CallStartMailbox::PeerHash pattern(uint32_t sequence) {
    CallStartMailbox::PeerHash result{};
    for (size_t i = 0; i < result.size(); ++i) {
        const uint32_t shift = static_cast<uint32_t>((i % 4) * 8);
        result[i] = static_cast<uint8_t>((sequence >> shift) ^
                                         (0x5au + 37u * i));
    }
    return result;
}

static void roundtrip_preserves_all_16_bytes() {
    CallStartMailbox mailbox;
    const auto sent = pattern(0x1234abcdu);
    CallStartMailbox::PeerHash received{};
    EXPECT_TRUE(mailbox.request(sent));
    EXPECT_TRUE(mailbox.take(received));
    EXPECT_EQ(received, sent);
}

static void duplicate_pending_is_rejected_and_preserves_first() {
    CallStartMailbox mailbox;
    const auto first = pattern(1);
    const auto second = pattern(2);
    CallStartMailbox::PeerHash received{};
    EXPECT_TRUE(mailbox.request(first));
    EXPECT_TRUE(!mailbox.request(second));
    EXPECT_TRUE(mailbox.take(received));
    EXPECT_EQ(received, first);
}

static void empty_take_leaves_output_unchanged() {
    CallStartMailbox mailbox;
    auto output = pattern(99);
    const auto before = output;
    EXPECT_TRUE(!mailbox.take(output));
    EXPECT_EQ(output, before);
}

static void producer_consumer_stress_has_no_torn_payload() {
    CallStartMailbox mailbox;
    constexpr uint32_t total = 100000;
    std::atomic<uint32_t> errors{0};
    std::atomic<uint32_t> consumed{0};

    std::thread producer([&] {
        for (uint32_t sequence = 1; sequence <= total; ++sequence) {
            const auto payload = pattern(sequence);
            while (!mailbox.request(payload)) std::this_thread::yield();
        }
    });

    std::thread consumer([&] {
        uint32_t expected = 1;
        while (expected <= total) {
            CallStartMailbox::PeerHash received{};
            if (!mailbox.take(received)) {
                std::this_thread::yield();
                continue;
            }
            if (received != pattern(expected)) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            ++expected;
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(errors.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(consumed.load(std::memory_order_relaxed), total);
}

int main() {
    RUN(roundtrip_preserves_all_16_bytes);
    RUN(duplicate_pending_is_rejected_and_preserves_first);
    RUN(empty_take_leaves_output_unchanged);
    RUN(producer_consumer_stress_has_no_torn_payload);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
