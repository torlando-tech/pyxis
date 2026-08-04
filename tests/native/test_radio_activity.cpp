#include <cstdint>
#include <iostream>

#include "RadioActivityHistory.h"

using RadioActivity::Event;
using RadioActivity::History;
using RadioActivity::has_event;

int main() {
    int passed = 0;
    int failed = 0;

    auto check = [&](const char* name, bool condition) {
        if (condition) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "FAIL: " << name << "\n";
        }
    };

    History history;
    for (std::size_t i = 0; i < History::CAPACITY + 5; ++i) {
        history.record(-130 + static_cast<int16_t>(i % 3));
    }
    auto bounded = history.snapshot();
    check("history has fixed capacity", bounded.count == History::CAPACITY);
    check("snapshot is chronological after wrap",
          bounded.samples[0].sequence == 5 &&
          bounded.samples[bounded.count - 1].sequence == History::CAPACITY + 4);

    History baseline;
    baseline.record(-121);
    baseline.record(-120);
    baseline.record(-119);
    baseline.record(-120);
    auto learned = baseline.snapshot();
    check("noise floor becomes ready from accepted samples", learned.noise_floor_ready);
    check("noise floor learns a stable baseline", learned.noise_floor == -120);

    baseline.record(-95);
    auto interference = baseline.snapshot();
    check("sample above noise threshold is interference",
          has_event(interference.samples[interference.count - 1], Event::Interference));
    check("interference does not raise learned noise floor", interference.noise_floor == -120);

    baseline.record(-112);
    auto accepted = baseline.snapshot();
    check("sample within eleven dB is accepted as noise",
          accepted.samples[accepted.count - 1].events == 0);
    check("accepted sample updates bounded noise estimator", accepted.noise_floor > -120);

    History events;
    events.mark_event(Event::Tx);
    events.mark_event(Event::Rx);
    check("events do not append off-cadence samples", events.snapshot().count == 0);
    events.record(-118);
    auto marked = events.snapshot();
    check("events share one fixed-cadence bucket", marked.count == 1);
    check("transmit metadata survives until the next bucket", has_event(marked.samples[0], Event::Tx));
    check("receive metadata shares the next bucket", has_event(marked.samples[0], Event::Rx));
    check("channel load counts activity", marked.channel_load_percent == 100);

    History clamped;
    clamped.record(-200);
    clamped.record(20);
    auto limits = clamped.snapshot();
    check("RSSI is clamped to graph bounds",
          limits.samples[0].rssi_dbm == History::MIN_RSSI_DBM &&
          limits.samples[1].rssi_dbm == History::MAX_RSSI_DBM);
    check("channel load is bounded", limits.channel_load_percent <= 100);

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
