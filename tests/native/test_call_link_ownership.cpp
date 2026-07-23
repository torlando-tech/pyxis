#include "../../lib/tdeck_ui/UI/LXMF/CallLinkOwnership.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>

using UI::LXMF::CallLinkOwnership;

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

static CallLinkOwnership::LinkId id(uint8_t first, uint8_t last) {
    CallLinkOwnership::LinkId result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<uint8_t>(first + i);
    }
    result.back() = last;
    return result;
}

static void replacement_rejects_all_stale_id_generation_pairs() {
    CallLinkOwnership ownership;
    const auto a = id(0x10, 0xa1);
    const auto b = id(0x10, 0xb2); // differs only in the final ID byte

    EXPECT_TRUE(ownership.publish(1, a));
    EXPECT_TRUE(ownership.owns(1, a));
    EXPECT_EQ(ownership.generationFor(a), 1u);

    EXPECT_TRUE(ownership.publish(2, b));
    EXPECT_TRUE(!ownership.owns(1, a));
    EXPECT_TRUE(!ownership.owns(1, b));
    EXPECT_TRUE(!ownership.owns(2, a));
    EXPECT_TRUE(ownership.owns(2, b));
    EXPECT_EQ(ownership.generationFor(a), 0u);
    EXPECT_EQ(ownership.generationFor(b), 2u);
}

static void distinct_non_null_synthetic_links_require_exact_full_id() {
    CallLinkOwnership ownership;
    const auto a = id(0x40, 0xee);
    auto b = a;
    b[15] ^= 1u;

    // Both IDs model links whose bool conversion is true. Their exact 128-bit
    // identities, not that shared truth value or a truncated prefix, decide.
    EXPECT_TRUE(ownership.publish(7, a));
    EXPECT_TRUE(ownership.owns(7, a));
    EXPECT_TRUE(!ownership.owns(7, b));
}

static void stale_close_cannot_set_or_overwrite_new_owner_slot() {
    CallLinkOwnership ownership;
    const auto a = id(0x20, 0xa1);
    const auto b = id(0x30, 0xb2);

    EXPECT_TRUE(ownership.publish(1, a));
    EXPECT_TRUE(ownership.owns(1, a)); // old callback validated, then paused
    EXPECT_TRUE(ownership.publish(2, b));
    EXPECT_TRUE(!ownership.markClosed(1, a));
    EXPECT_EQ(ownership.takeClosed(), 0u);

    EXPECT_TRUE(ownership.markClosed(2, b));
    EXPECT_TRUE(!ownership.markClosed(1, a));
    EXPECT_EQ(ownership.takeClosed(), 2u);
    EXPECT_EQ(ownership.takeClosed(), 0u);
}

static void current_close_survives_stale_attempts_before_and_after() {
    CallLinkOwnership ownership;
    const auto a = id(0x51, 0xa1);
    const auto b = id(0x61, 0xb2);

    EXPECT_TRUE(ownership.publish(1, a));
    EXPECT_TRUE(ownership.publish(2, b));
    EXPECT_TRUE(!ownership.markClosed(1, a));
    EXPECT_TRUE(ownership.markClosed(2, b));
    EXPECT_TRUE(!ownership.markClosed(1, a));
    EXPECT_EQ(ownership.takeClosed(), 2u);
}

static void stale_clear_cannot_detach_or_clear_new_owner_close() {
    CallLinkOwnership ownership;
    const auto a = id(0x71, 0xa1);
    const auto b = id(0x81, 0xb2);

    EXPECT_TRUE(ownership.publish(1, a));
    EXPECT_TRUE(ownership.clear(1));
    EXPECT_TRUE(ownership.publish(2, b));
    EXPECT_TRUE(ownership.markClosed(2, b));
    EXPECT_TRUE(!ownership.clear(1));
    EXPECT_TRUE(ownership.owns(2, b));
    EXPECT_EQ(ownership.takeClosed(), 2u);
}

static void concurrent_stale_and_current_close_publication_is_exact() {
    constexpr int kIterations = 1000;
    const auto a = id(0x91, 0xa1);
    const auto b = id(0xa1, 0xb2);

    for (int iteration = 0; iteration < kIterations; ++iteration) {
        CallLinkOwnership ownership;
        EXPECT_TRUE(ownership.publish(1, a));
        EXPECT_TRUE(ownership.publish(2, b));

        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        bool stale_result = true;
        bool current_result = false;
        auto mark = [&](uint32_t generation,
                        const CallLinkOwnership::LinkId& link_id,
                        bool& result) {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            result = ownership.markClosed(generation, link_id);
        };

        std::thread stale(mark, 1u, std::cref(a), std::ref(stale_result));
        std::thread current(mark, 2u, std::cref(b), std::ref(current_result));
        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        stale.join();
        current.join();

        EXPECT_TRUE(!stale_result);
        EXPECT_TRUE(current_result);
        EXPECT_EQ(ownership.takeClosed(), 2u);
    }
}

static void invalid_generations_are_never_published() {
    CallLinkOwnership ownership;
    const auto a = id(0xb1, 0xc2);
    EXPECT_TRUE(!ownership.publish(0, a));
    EXPECT_TRUE(!ownership.publish(CallLinkOwnership::MAX_GENERATION + 1u, a));
    EXPECT_EQ(ownership.generationFor(a), 0u);
}

static void concurrent_publication_never_accepts_a_torn_128_bit_id() {
    CallLinkOwnership ownership;
    CallLinkOwnership::LinkId a{};
    CallLinkOwnership::LinkId b{};
    CallLinkOwnership::LinkId torn{};
    a.fill(0x11);
    b.fill(0xee);
    for (size_t i = 0; i < torn.size(); ++i) {
        torn[i] = i < torn.size() / 2 ? a[i] : b[i];
    }
    EXPECT_TRUE(ownership.publish(1, a));

    std::atomic<bool> done{false};
    std::atomic<bool> accepted_torn{false};
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (ownership.generationFor(torn) != 0) {
                accepted_torn.store(true, std::memory_order_release);
                return;
            }
        }
    });
    for (uint32_t generation = 2; generation < 100002; ++generation) {
        EXPECT_TRUE(ownership.publish(
            generation, (generation & 1u) == 0 ? b : a));
    }
    done.store(true, std::memory_order_release);
    reader.join();
    EXPECT_TRUE(!accepted_torn.load(std::memory_order_acquire));
}

int main() {
    RUN(replacement_rejects_all_stale_id_generation_pairs);
    RUN(distinct_non_null_synthetic_links_require_exact_full_id);
    RUN(stale_close_cannot_set_or_overwrite_new_owner_slot);
    RUN(current_close_survives_stale_attempts_before_and_after);
    RUN(stale_clear_cannot_detach_or_clear_new_owner_close);
    RUN(concurrent_stale_and_current_close_publication_is_exact);
    RUN(invalid_generations_are_never_published);
    RUN(concurrent_publication_never_accepts_a_torn_128_bit_id);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
