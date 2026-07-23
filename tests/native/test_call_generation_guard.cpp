#include "../../lib/tdeck_ui/UI/LXMF/CallGenerationGuard.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>

using UI::LXMF::CallGenerationGuard;

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

static void first_reservation_is_nonzero_and_owned() {
    CallGenerationGuard guard;
    const uint32_t generation = guard.tryReserve();
    EXPECT_TRUE(generation != 0);
    EXPECT_TRUE(generation <= CallGenerationGuard::MAX_GENERATION);
    EXPECT_TRUE(guard.owns(generation));
    EXPECT_EQ(guard.current(), generation);
}

static void reservation_while_owned_fails() {
    CallGenerationGuard guard;
    const uint32_t generation = guard.tryReserve();
    EXPECT_TRUE(generation != 0);
    EXPECT_EQ(guard.tryReserve(), 0u);
    EXPECT_EQ(guard.current(), generation);
}

static void zero_and_stale_release_do_not_clear_owner() {
    CallGenerationGuard guard;
    const uint32_t generation = guard.tryReserve();
    EXPECT_TRUE(!guard.release(0));
    EXPECT_TRUE(!guard.release(generation + 1));
    EXPECT_TRUE(guard.owns(generation));
}

static void active_release_clears_owner() {
    CallGenerationGuard guard;
    const uint32_t generation = guard.tryReserve();
    EXPECT_TRUE(guard.release(generation));
    EXPECT_EQ(guard.current(), 0u);
    EXPECT_TRUE(!guard.owns(generation));
    EXPECT_TRUE(!guard.release(generation));
}

static void next_reservation_has_distinct_generation() {
    CallGenerationGuard guard;
    const uint32_t first = guard.tryReserve();
    EXPECT_TRUE(guard.release(first));
    const uint32_t second = guard.tryReserve();
    EXPECT_TRUE(second != 0);
    EXPECT_TRUE(second != first);
}

static void exactly_one_thread_wins_reservation_race() {
    constexpr int kRaceIterations = 1000;
    for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
        CallGenerationGuard guard;
        std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false};
        uint32_t first = 0;
        uint32_t second = 0;

        auto reserve = [&](uint32_t& result) {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            result = guard.tryReserve();
        };

        std::thread a(reserve, std::ref(first));
        std::thread b(reserve, std::ref(second));
        while (ready.load(std::memory_order_acquire) != 2) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        a.join();
        b.join();

        EXPECT_TRUE((first == 0) != (second == 0));
        EXPECT_EQ(guard.current(), first != 0 ? first : second);
    }
}

static void stale_token_cannot_release_new_winner() {
    CallGenerationGuard guard;
    const uint32_t stale = guard.tryReserve();
    EXPECT_TRUE(guard.release(stale));
    const uint32_t winner = guard.tryReserve();
    EXPECT_TRUE(winner != stale);
    EXPECT_TRUE(!guard.release(stale));
    EXPECT_TRUE(guard.owns(winner));
}

static void incoming_then_outgoing_admission_sequence() {
    CallGenerationGuard guard;
    const uint32_t incoming = guard.tryReserve();
    EXPECT_TRUE(incoming != 0);
    EXPECT_EQ(guard.tryReserve(), 0u); // outgoing rejected while incoming owns it
    EXPECT_TRUE(guard.release(incoming));

    const uint32_t outgoing = guard.tryReserve();
    EXPECT_TRUE(outgoing != 0);
    EXPECT_TRUE(outgoing != incoming);
    EXPECT_TRUE(!guard.release(incoming)); // late incoming cleanup is harmless
    EXPECT_TRUE(guard.owns(outgoing));
    EXPECT_TRUE(guard.release(outgoing));
    EXPECT_EQ(guard.current(), 0u);
}

int main() {
    RUN(first_reservation_is_nonzero_and_owned);
    RUN(reservation_while_owned_fails);
    RUN(zero_and_stale_release_do_not_clear_owner);
    RUN(active_release_clears_owner);
    RUN(next_reservation_has_distinct_generation);
    RUN(exactly_one_thread_wins_reservation_race);
    RUN(stale_token_cannot_release_new_winner);
    RUN(incoming_then_outgoing_admission_sequence);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
