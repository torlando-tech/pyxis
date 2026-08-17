// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT
#ifndef UI_LXMF_NOMADNET_CACHE_FLOW_H
#define UI_LXMF_NOMADNET_CACHE_FLOW_H

#include "NomadNetCache.h"

namespace UI { namespace LXMF { namespace NomadNet {

enum class CacheFlowState : std::uint8_t {
    IDLE,
    LOOKUP,
    INVALIDATE,
    NEED_LIVE,
    READY,
    FAILED,
    CANCELLED
};

class NomadNetCacheFlow {
public:
    explicit NomadNetCacheFlow(NomadNetCache& cache) : cache_(cache) {}

    CacheFlowState begin(const CacheKey&, std::uint64_t now, bool reload);
    void service();
    bool acceptLive(const std::vector<std::uint8_t>&, const CacheEligibility&,
                    std::uint64_t now);
    void cancel();
    CacheFlowState state() const { return state_; }
    bool pageReady() const { return state_ == CacheFlowState::READY && !page_.empty(); }
    bool takePage(ExternalVector<std::uint8_t>&);
    const std::string& status() const { return status_; }

private:
    NomadNetCache& cache_;
    CacheKey key_;
    CacheFlowState state_ = CacheFlowState::IDLE;
    ExternalVector<std::uint8_t> page_;
    std::string status_;
    std::uint64_t now_ = 0;
    bool invalidation_admitted_ = false;
};

}}} // namespace UI::LXMF::NomadNet
#endif
