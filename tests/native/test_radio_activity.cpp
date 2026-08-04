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

    History skipped;
    skipped.mark_event(Event::Tx, 3);
    skipped.record_gap();
    skipped.record_gap();
    skipped.record_gap();
    auto tx_gap = skipped.snapshot();
    check("skipped cadence buckets are represented", tx_gap.count == 3);
    check("TX duration spans every queued bucket",
          has_event(tx_gap.samples[0], Event::Tx) &&
          has_event(tx_gap.samples[1], Event::Tx) &&
          has_event(tx_gap.samples[2], Event::Tx));
    check("gap buckets do not invent RSSI", !tx_gap.samples[0].rssi_valid);
    check("TX gap buckets contribute to load", tx_gap.channel_load_percent == 100);

    History aligned;
    aligned.mark_event(Event::Tx, 2);
    aligned.record_gap(false);
    aligned.record_gap();
    aligned.record(-100);
    auto right_aligned = aligned.snapshot();
    check("pending duration does not mark older catch-up gaps",
          !has_event(right_aligned.samples[0], Event::Tx));
    check("pending duration is right-aligned to current time",
          has_event(right_aligned.samples[1], Event::Tx) &&
          has_event(right_aligned.samples[2], Event::Tx));

    History rx_burst;
    rx_burst.mark_event(Event::Rx);
    rx_burst.mark_event(Event::Rx);
    rx_burst.record(-110);
    rx_burst.record(-120);
    auto rx_bucket = rx_burst.snapshot();
    check("multiple RX events coalesce within one cadence bucket",
          has_event(rx_bucket.samples[0], Event::Rx));
    check("coalesced RX does not spill into a future bucket",
          !has_event(rx_bucket.samples[1], Event::Rx));

    History unknown_gap;
    unknown_gap.record_gap();
    auto unknown = unknown_gap.snapshot();
    check("unknown contention gap advances time without activity",
          unknown.count == 1 && unknown.channel_load_percent == 0);

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
