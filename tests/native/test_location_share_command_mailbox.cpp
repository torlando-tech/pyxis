#include "UI/LXMF/LocationShareCommandMailbox.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

using UI::LXMF::LocationShareCommandMailbox;
static int pass = 0, fail = 0;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
#define RUN(x) do { try { x(); ++pass; } catch (...) { ++fail; } } while (0)

static void exact_peer_is_required() {
    LocationShareCommandMailbox box;
    uint8_t peer[17] = {};
    CHECK(!box.requestStart(peer, 15, 0, 60000, false, 0));
    CHECK(!box.requestStop(peer, 17));
    CHECK(box.take().action == LocationShareCommandMailbox::Action::NONE);
}
static void start_is_fixed_and_one_shot() {
    LocationShareCommandMailbox box;
    uint8_t peer[16]; for (int i=0;i<16;++i) peer[i]=static_cast<uint8_t>(i);
    CHECK(box.requestStart(peer, 16, 2, 300000, true, 1000));
    CHECK(!box.requestStop(peer, 16));
    const LocationShareCommandMailbox::Command c = box.take();
    CHECK(c.action == LocationShareCommandMailbox::Action::START);
    CHECK(std::memcmp(c.peer, peer, 16) == 0);
    CHECK(c.duration == 2 && c.cadenceMillis == 300000 && c.hasApproximation && c.approximationMeters == 1000);
    CHECK(box.take().action == LocationShareCommandMailbox::Action::NONE);
}
static void stop_is_immediate_command() {
    LocationShareCommandMailbox box;
    uint8_t peer[16] = {};
    CHECK(box.requestStop(peer, 16));
    CHECK(box.take().action == LocationShareCommandMailbox::Action::STOP);
}
static void query_is_peer_scoped_and_non_evicting() {
    LocationShareCommandMailbox box;
    uint8_t peer[16] = {};
    CHECK(box.requestQuery(peer, 16));
    CHECK(!box.requestStop(peer, 16));
    const LocationShareCommandMailbox::Command c = box.take();
    CHECK(c.action == LocationShareCommandMailbox::Action::QUERY);
    CHECK(std::memcmp(c.peer, peer, 16) == 0);
}
int main() { RUN(exact_peer_is_required); RUN(start_is_fixed_and_one_shot); RUN(stop_is_immediate_command); RUN(query_is_peer_scoped_and_non_evicting); std::printf("location mailbox: %d passed, %d failed\n", pass, fail); return fail; }
