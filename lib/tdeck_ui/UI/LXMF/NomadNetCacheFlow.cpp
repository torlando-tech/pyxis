// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT
#include "NomadNetCacheFlow.h"

namespace UI { namespace LXMF { namespace NomadNet {

CacheFlowState NomadNetCacheFlow::begin(const CacheKey& key, std::uint64_t now,
                                        bool reload) {
    cancel();
    key_ = key;
    now_ = now;
    ExternalVector<std::uint8_t>().swap(page_);
    status_ = reload ? "Invalidating cached page..." : "Checking page cache...";
    invalidation_admitted_ = false;
    if (reload) {
        state_ = CacheFlowState::INVALIDATE;
        if (!cache_.busy()) {
            invalidation_admitted_ = cache_.invalidate(key_) == CacheResult::PENDING;
        }
        return state_;
    }
    cache_.beginLookup(key_, now, false);
    state_ = cache_.busy() ? CacheFlowState::LOOKUP : CacheFlowState::NEED_LIVE;
    if (state_ == CacheFlowState::NEED_LIVE) status_ = "Requesting page...";
    return state_;
}

void NomadNetCacheFlow::service() {
    if (state_ == CacheFlowState::CANCELLED || state_ == CacheFlowState::FAILED)
        return;
    if (cache_.busy()) cache_.service();
    if (state_ == CacheFlowState::INVALIDATE) {
        if (cache_.busy()) return;
        if (!invalidation_admitted_) {
            invalidation_admitted_ = cache_.invalidate(key_) == CacheResult::PENDING;
            return;
        }
        if (cache_.lastResult() == CacheResult::MISS) {
            state_ = CacheFlowState::NEED_LIVE;
            status_ = "Requesting page...";
        } else {
            state_ = CacheFlowState::FAILED;
            status_ = "Page cache invalidation failed";
        }
        return;
    }
    if (state_ != CacheFlowState::LOOKUP || cache_.busy()) return;
    if (cache_.lastResult() == CacheResult::HIT && cache_.takeBody(page_)) {
        state_ = CacheFlowState::READY;
        status_ = "Cached page; current reachability not checked";
    } else {
        state_ = CacheFlowState::NEED_LIVE;
        status_ = "Requesting page...";
    }
}

bool NomadNetCacheFlow::acceptLive(const std::vector<std::uint8_t>& body,
                                   const CacheEligibility& eligibility,
                                   std::uint64_t now) {
    if (state_ != CacheFlowState::NEED_LIVE || !eligibility.successful ||
        !eligibility.valid || eligibility.partial || eligibility.malformed ||
        eligibility.truncated || eligibility.error || body.empty()) {
        state_ = CacheFlowState::FAILED;
        status_ = "Malformed NomadNet response";
        return false;
    }
    try {
        page_.assign(body.begin(), body.end());
    } catch (const std::bad_alloc&) {
        state_ = CacheFlowState::FAILED;
        status_ = "Page is too large for available memory";
        return false;
    }
    state_ = CacheFlowState::READY;
    status_ = "Page loaded (live)";
    if (cache_eligible(eligibility)) {
        const auto directive = parse_cache_directive(body.data(), body.size());
        if (directive.valid && directive.ttl)
            cache_.beginCommit(key_, body, now, directive.ttl);
        else
            cache_.invalidate(key_);
    }
    return true;
}

void NomadNetCacheFlow::cancel() {
    if (cache_.busy()) cache_.cancel();
    ExternalVector<std::uint8_t>().swap(page_);
    if (state_ != CacheFlowState::IDLE) state_ = CacheFlowState::CANCELLED;
    status_.clear();
}

bool NomadNetCacheFlow::takePage(ExternalVector<std::uint8_t>& output) {
    if (!pageReady()) return false;
    output.swap(page_);
    return true;
}

}}} // namespace UI::LXMF::NomadNet
