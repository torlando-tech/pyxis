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
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace UI {
namespace LXMF {

struct UnknownSourceKeyRequestPolicy {
    static constexpr unsigned long long kCooldownMillis = 5U * 60U * 1000U;
    static constexpr unsigned kMaxTrackedSources = 64U;

    std::vector<std::pair<std::string, unsigned long long>> last_requested;

    bool should_request(const std::string& source_hex,
                        unsigned long long now_millis) const {
        for (const auto& entry : last_requested) {
            if (entry.first == source_hex) {
                return now_millis >=
                       entry.second + kCooldownMillis;
            }
        }
        return true;
    }

    void record_request(const std::string& source_hex,
                        unsigned long long now_millis) {
        for (auto& entry : last_requested) {
            if (entry.first == source_hex) {
                entry.second = now_millis;
                return;
            }
        }
        if (last_requested.size() >= kMaxTrackedSources) {
            last_requested.erase(last_requested.begin());
        }
        last_requested.emplace_back(source_hex, now_millis);
    }
};

}  // namespace LXMF
}  // namespace UI
