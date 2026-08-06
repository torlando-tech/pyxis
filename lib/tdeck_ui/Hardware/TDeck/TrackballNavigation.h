// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <limits>
#include <cstddef>

namespace Hardware::TDeck {

enum class NavigationDirection : uint8_t { UP, DOWN, LEFT, RIGHT };

struct NavigationRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct NavigationCandidate {
    int id;
    NavigationRect rect;
    bool visible;
};

/**
 * Select the nearest focus target in the requested physical direction.
 *
 * Perpendicular distance is weighted so rolling vertically stays in the same
 * visual column and rolling horizontally stays in the same row. This avoids
 * depending on the order controls happened to be added to an LVGL group.
 */
inline int64_t directional_candidate_score(
    const NavigationRect& focused,
    const NavigationCandidate& candidate,
    NavigationDirection direction) {
    if (!candidate.visible) return std::numeric_limits<int64_t>::max();
    const int64_t focused_x = static_cast<int64_t>(focused.x) + focused.width / 2;
    const int64_t focused_y = static_cast<int64_t>(focused.y) + focused.height / 2;
    const int64_t candidate_x = static_cast<int64_t>(candidate.rect.x) + candidate.rect.width / 2;
    const int64_t candidate_y = static_cast<int64_t>(candidate.rect.y) + candidate.rect.height / 2;
    const int64_t dx = candidate_x - focused_x;
    const int64_t dy = candidate_y - focused_y;

    int64_t primary = 0;
    int64_t perpendicular = 0;
    switch (direction) {
        case NavigationDirection::UP:
            if (dy >= 0) return std::numeric_limits<int64_t>::max();
            primary = -dy;
            perpendicular = dx < 0 ? -dx : dx;
            break;
        case NavigationDirection::DOWN:
            if (dy <= 0) return std::numeric_limits<int64_t>::max();
            primary = dy;
            perpendicular = dx < 0 ? -dx : dx;
            break;
        case NavigationDirection::LEFT:
            if (dx >= 0) return std::numeric_limits<int64_t>::max();
            primary = -dx;
            perpendicular = dy < 0 ? -dy : dy;
            break;
        case NavigationDirection::RIGHT:
            if (dx <= 0) return std::numeric_limits<int64_t>::max();
            primary = dx;
            perpendicular = dy < 0 ? -dy : dy;
            break;
    }
    return primary + perpendicular * 2;
}

inline int select_directional_candidate(
    const NavigationRect& focused,
    const NavigationCandidate* candidates,
    std::size_t candidate_count,
    NavigationDirection direction) {
    int best_id = -1;
    int64_t best_score = std::numeric_limits<int64_t>::max();

    for (std::size_t i = 0; i < candidate_count; ++i) {
        const auto& candidate = candidates[i];
        const int64_t score = directional_candidate_score(focused, candidate, direction);
        if (score < best_score || (score == best_score && candidate.id < best_id)) {
            best_score = score;
            best_id = candidate.id;
        }
    }
    return best_id;
}

} // namespace Hardware::TDeck
