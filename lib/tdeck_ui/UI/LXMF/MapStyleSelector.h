// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef PYXIS_UI_LXMF_MAP_STYLE_SELECTOR_H
#define PYXIS_UI_LXMF_MAP_STYLE_SELECTOR_H

#include <cstddef>
#include <cstdint>

#include "UI/LXMF/MapPackManifest.h"

namespace Pyxis {

struct MapStyleSummary {
    char id[MapPackManifest::PACK_ID_CAPACITY];
    char label[20];
};

struct MapStyleRequest {
    std::uint32_t token;
    std::uint32_t catalog_generation;
    char style_id[MapPackManifest::PACK_ID_CAPACITY];
};

struct MapStyleCompletion {
    std::uint32_t token;
    std::uint32_t catalog_generation;
    char style_id[MapPackManifest::PACK_ID_CAPACITY];
    bool success;
};

/** Allocation-free UI state for one bounded map-style activation intent. */
class MapStyleSelector {
public:
    static const std::size_t MAX_STYLES = 4U;
    enum class State : std::uint8_t { DISCOVERING, READY, APPLYING, ERROR };

    MapStyleSelector();

    bool setCatalog(std::uint32_t generation, const MapStyleSummary* styles,
                    std::size_t count, const char* active_id);
    bool requestNext(MapStyleRequest& output);
    bool complete(const MapStyleCompletion& completion);

    State state() const { return state_; }
    std::size_t count() const { return count_; }
    bool canCycle() const {
        return (count_ > 1U || (count_ == 1U && active_index_ < 0)) &&
            state_ != State::APPLYING;
    }
    const char* activeId() const;
    const char* activeLabel() const;
    std::uint32_t generation() const { return catalog_generation_; }

private:
    MapStyleSummary styles_[MAX_STYLES];
    std::size_t count_;
    std::ptrdiff_t active_index_;
    std::size_t pending_index_;
    std::uint32_t catalog_generation_;
    std::uint32_t next_token_;
    std::uint32_t pending_token_;
    State state_;

    static bool validString(const char* value, std::size_t capacity, bool identifier);
    static std::uint32_t advance(std::uint32_t value);
};

}  // namespace Pyxis

#endif
