#pragma once

#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "NomadNetDocument.h"
#include "NomadNetHistory.h"
#include "NomadNetLibrary.h"
#include "NomadNetUrl.h"

namespace UI::LXMF::NomadNet {

enum class LocalNavigationResult : uint8_t {
    APPLIED,
    STAGING_FAILED,
    PREPARATION_FAILED,
    PUBLICATION_FAILED,
    SUPERSEDED,
};

// Shared production seam for same-resource navigation. OwnerController may have
// already staged BACK; in that case this function must not replace it with OPEN.
// Transport/canonical preparation happens before visible publication, and the
// staged history transition is committed only after publication succeeds.
template <typename PreparePublication, typename Publisher>
LocalNavigationResult apply_local_navigation_transaction(
        const std::string& canonical_address, bool add_history,
        int32_t current_scroll, int32_t restore_scroll, bool history_prepared,
        PageHistory& history, PageHistory::PendingOpen& pending,
        PreparePublication&& prepare_publication, Publisher&& publish) noexcept {
    try {
        if (history_prepared) {
            if (!pending.ready() || pending.address() != canonical_address) {
                pending.clear();
                return LocalNavigationResult::SUPERSEDED;
            }
        } else {
            const auto& request = history.current_request_data();
            const uint8_t* request_bytes = history.current_has_request_data()
                ? request.data() : nullptr;
            const std::size_t request_size = history.current_has_request_data()
                ? request.size() : 0;
            if (!history.prepare_open(canonical_address, add_history, current_scroll,
                    request_bytes, request_size, pending)) {
                pending.clear();
                return LocalNavigationResult::STAGING_FAILED;
            }
        }
        if (!prepare_publication()) {
            pending.clear();
            return LocalNavigationResult::PREPARATION_FAILED;
        }
        if (!publish(canonical_address, restore_scroll)) {
            pending.clear();
            return LocalNavigationResult::PUBLICATION_FAILED;
        }
        if (!history.commit(std::move(pending))) {
            pending.clear();
            return LocalNavigationResult::SUPERSEDED;
        }
        return LocalNavigationResult::APPLIED;
    } catch (const std::bad_alloc&) {
        pending.clear();
        return LocalNavigationResult::STAGING_FAILED;
    }
}

enum class PageApplyResult : uint8_t {
    APPLIED,
    ALLOCATION_FAILED,
    PUBLICATION_FAILED,
};

struct PagePublication {
    const Library& library;
    bool library_changed;
    bool page_saved;
    bool identify_enabled;
    const std::string& unknown_anchor_status;
};

// The production application transaction. All heading/title/library preparation
// is staged, publication is attempted against the still-visible prior page, and
// durable owner state is finalized only after publication succeeds.
template <typename Publisher, typename Finalizer>
PageApplyResult apply_page_transaction(const Document& document,
        const std::string& path, const std::string& url,
        const std::string& destination_hex, const std::string& fragment,
        uint64_t timestamp, Library& library, Publisher&& publish,
        Finalizer&& finalize) noexcept {
    try {
        std::vector<std::string> heading_runs;
        for (const auto& block : document.blocks) {
            if (block.type != BlockType::HEADING) continue;
            heading_runs.reserve(block.runs.size());
            for (const auto& run : block.runs) heading_runs.push_back(run.text);
            break;
        }
        const std::string title = page_title(path, heading_runs);
        Library candidate = library;
        const bool changed = candidate.record_page(url, title, timestamp);
        const bool saved = candidate.page_saved(url);
        const bool identified = candidate.node_identified(destination_hex);
        const std::string anchor_status = fragment.empty()
            ? std::string() : "Unknown anchor: #" + fragment;
        const PagePublication publication{
            candidate, changed, saved, identified, anchor_status};
        if (!publish(publication)) return PageApplyResult::PUBLICATION_FAILED;
        finalize();
        library = std::move(candidate);
        return PageApplyResult::APPLIED;
    } catch (const std::bad_alloc&) {
        return PageApplyResult::ALLOCATION_FAILED;
    }
}

// Canonical URL construction can allocate. Keep it inside the same noexcept
// boundary as page staging so UIManager cannot leak bad_alloc before apply.
template <typename Publisher, typename Finalizer>
PageApplyResult apply_page_transaction_for_url(const Document& document,
        const Url& url, uint64_t timestamp, Library& library,
        Publisher&& publish, Finalizer&& finalize) noexcept {
    try {
        const std::string canonical_url = url.str();
        return apply_page_transaction(document, url.path, canonical_url,
            url.destination_hex, url.fragment, timestamp, library,
            std::forward<Publisher>(publish), std::forward<Finalizer>(finalize));
    } catch (const std::bad_alloc&) {
        return PageApplyResult::ALLOCATION_FAILED;
    }
}

} // namespace UI::LXMF::NomadNet
