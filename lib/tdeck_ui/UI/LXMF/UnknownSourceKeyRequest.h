#pragma once

// ---------------------------------------------------------------------------
// On-demand identity acquisition policy for unknown LXMF sources.
//
// When an LXMF message is delivered to us from a source whose RNS identity
// has not been learned, the router accepts the message (microLXMF treats
// SOURCE_UNKNOWN as "will validate later if the identity is learned via
// announce") but location ingest is skipped for unauthenticated senders.
// Nothing ever triggered that learning, so an unknown peer stayed unknown
// until — and unless — a natural announce happened to arrive.
//
// This policy mirrors Sideband's "Query Network For Keys" button
// (RNS.Transport.request_path): the caller fires a broadcast RNS path
// request for the source hash, and any peer that already holds the
// source's cached announce (a phone on the same link, a hub, another node)
// answers with it. The next message from that peer then validates.
//
// The decision is pure and host-testable; the Transport::request_path call
// stays in the UI layer (see UIManager::on_message_received).
//
// Only SOURCE_UNKNOWN should ever be passed here: an invalid signature
// from a KNOWN identity is not recoverable by asking the network for the
// same key it already has.
//
// Rate limit, two independent bounds:
//   1. Per-identity: at most one automatic path request per unknown
//      identity per 30 minutes. Once the first request fires, the source's
//      window is tracked and never dropped before it expires, so no
//      identity can force a second request within its window — an
//      eviction of one source cannot reset another's cooldown.
//   2. Aggregate: at most kMaxTrackedSources automatic requests per
//      rolling 30-minute window in total (a request budget that expires
//      with the requests it counts). While the budget is exhausted,
//      never-before-seen identities are declined even though their own
//      per-identity window is empty; once their budget slots free up they
//      may fire. This caps the worst-case network cost of a flood of
//      rotating bogus identities at kMaxTrackedSources path requests per
//      window, instead of leaving the answer bandwidth unbounded.
//      Cost: a 65th honest new peer in a congested window is deferred by
//      at most one 30-minute window.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace UI {
namespace LXMF {

struct UnknownSourceKeyRequestPolicy {
    static constexpr unsigned long long kCooldownMillis = 30U * 60U * 1000U;
    // Aggregate bound: maximum number of automatic path requests in any
    // rolling 30-minute window. Also bounds the per-source tracking table
    // (one open window per fired request).
    static constexpr unsigned kMaxTrackedSources = 64U;

    struct Entry {
        std::string source_hex;
        unsigned long long last_requested_millis;
        Entry() : last_requested_millis(0) {}
    };

    // One entry per fired request whose window is still open.
    std::vector<Entry> last_requested;

    // Drop entries whose window has fully elapsed. O(n) with n <=
    // kMaxTrackedSources; called lazily on every decision.
    void prune_expired(unsigned long long now_millis) {
        for (unsigned i = 0; i < last_requested.size();) {
            if (now_millis >=
                last_requested[i].last_requested_millis + kCooldownMillis) {
                last_requested.erase(last_requested.begin() + i);
            } else {
                ++i;
            }
        }
    }

    bool should_request(const std::string& source_hex,
                        unsigned long long now_millis) {
        for (const auto& entry : last_requested) {
            if (entry.source_hex == source_hex) {
                return now_millis >=
                       entry.last_requested_millis + kCooldownMillis;
            }
        }
        // Not seen since its own window expired: the request is allowed
        // only if the aggregate budget has room, so a flood of distinct
        // identities cannot force more than kMaxTrackedSources requests
        // per window.
        prune_expired(now_millis);
        return last_requested.size() < kMaxTrackedSources;
    }

    void record_request(const std::string& source_hex,
                        unsigned long long now_millis) {
        for (auto& entry : last_requested) {
            if (entry.source_hex == source_hex) {
                entry.last_requested_millis = now_millis;
                return;
            }
        }
        prune_expired(now_millis);
        if (last_requested.size() >= kMaxTrackedSources) {
            // Defensive: should_request() gates this; if the budget is
            // full the request was not authorized and must not be
            // recorded (recording would extend the budget for an
            // unauthorized request).
            return;
        }
        Entry entry;
        entry.source_hex = source_hex;
        entry.last_requested_millis = now_millis;
        last_requested.push_back(entry);
    }
};

}  // namespace LXMF
}  // namespace UI
