#include "../../lib/tdeck_ui/UI/LXMF/CallCommandMailbox.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

using UI::LXMF::CallCommandMailbox;
using Action = CallCommandMailbox::Action;

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

static void zero_generation_is_ignored() {
    CallCommandMailbox mailbox;
    mailbox.requestHangup(0);
    mailbox.requestMute(0, true);
    const auto commands = mailbox.take();
    EXPECT_EQ(commands.hangupGeneration, 0u);
    EXPECT_EQ(commands.muteGeneration, 0u);
}

static void hangup_is_one_shot() {
    CallCommandMailbox mailbox;
    mailbox.requestHangup(17);
    EXPECT_EQ(mailbox.take().hangupGeneration, 17u);
    EXPECT_EQ(mailbox.take().hangupGeneration, 0u);
}

static void latest_mute_wins() {
    CallCommandMailbox mailbox;
    mailbox.requestMute(21, true);
    mailbox.requestMute(21, false);
    mailbox.requestMute(21, true);
    const auto commands = mailbox.take();
    EXPECT_EQ(commands.muteGeneration, 21u);
    EXPECT_TRUE(commands.muted);
    EXPECT_EQ(mailbox.take().muteGeneration, 0u);
}

static void hangup_and_mute_are_independent() {
    CallCommandMailbox mailbox;
    mailbox.requestHangup(31);
    mailbox.requestMute(31, true);
    const auto commands = mailbox.take();
    EXPECT_EQ(commands.hangupGeneration, 31u);
    EXPECT_EQ(commands.muteGeneration, 31u);
    EXPECT_TRUE(commands.muted);
}

static void stale_generation_is_discarded_by_consumer() {
    CallCommandMailbox mailbox;
    mailbox.requestHangup(40);
    mailbox.requestMute(40, false);
    EXPECT_EQ(mailbox.takeForGeneration(41).action, Action::NONE);
    EXPECT_EQ(mailbox.takeForGeneration(40).action, Action::NONE);
}

static void current_hangup_takes_precedence_over_mute() {
    CallCommandMailbox mailbox;
    mailbox.requestMute(51, true);
    mailbox.requestHangup(51);
    const auto command = mailbox.takeForGeneration(51);
    EXPECT_EQ(command.action, Action::HANGUP);
    EXPECT_EQ(mailbox.takeForGeneration(51).action, Action::NONE);
}

static void stale_hangup_does_not_hide_current_mute() {
    CallCommandMailbox mailbox;
    mailbox.requestHangup(60);
    mailbox.requestMute(61, true);
    const auto command = mailbox.takeForGeneration(61);
    EXPECT_EQ(command.action, Action::MUTE);
    EXPECT_TRUE(command.muted);
}

static void producer_consumer_stress() {
    CallCommandMailbox mailbox;
    constexpr uint32_t total = 100000;
    std::atomic<bool> done{false};
    std::atomic<uint32_t> observedMute{0};
    std::atomic<uint32_t> observedHangup{0};
    std::atomic<uint32_t> errors{0};

    std::thread producer([&] {
        for (uint32_t generation = 1; generation <= total; ++generation) {
            mailbox.requestMute(generation, (generation & 1u) != 0);
            if ((generation % 7u) == 0) mailbox.requestHangup(generation);
            if ((generation % 64u) == 0) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        for (;;) {
            const auto commands = mailbox.take();
            if (commands.muteGeneration != 0) {
                if (commands.muted != ((commands.muteGeneration & 1u) != 0)) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                observedMute.fetch_add(1, std::memory_order_relaxed);
            }
            if (commands.hangupGeneration != 0) {
                if ((commands.hangupGeneration % 7u) != 0) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                observedHangup.fetch_add(1, std::memory_order_relaxed);
            }
            if (done.load(std::memory_order_acquire)) {
                const auto finalCommands = mailbox.take();
                if (finalCommands.muteGeneration != 0) {
                    if (finalCommands.muted != ((finalCommands.muteGeneration & 1u) != 0)) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    observedMute.fetch_add(1, std::memory_order_relaxed);
                }
                if (finalCommands.hangupGeneration != 0) {
                    if ((finalCommands.hangupGeneration % 7u) != 0) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    observedHangup.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(errors.load(), 0u);
    EXPECT_TRUE(observedMute.load() > 0);
    EXPECT_TRUE(observedHangup.load() > 0);
}

int main() {
    RUN(zero_generation_is_ignored);
    RUN(hangup_is_one_shot);
    RUN(latest_mute_wins);
    RUN(hangup_and_mute_are_independent);
    RUN(stale_generation_is_discarded_by_consumer);
    RUN(current_hangup_takes_precedence_over_mute);
    RUN(stale_hangup_does_not_hide_current_mute);
    RUN(producer_consumer_stress);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
