#include <cstdlib>
#include <iostream>
#include <vector>

#include "../../lib/tdeck_ui/Hardware/TDeck/TrackballNavigation.h"

using Hardware::TDeck::NavigationCandidate;
using Hardware::TDeck::NavigationDirection;
using Hardware::TDeck::NavigationRect;
using Hardware::TDeck::select_directional_candidate;

static int failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    const auto actual_value = (actual); \
    const auto expected_value = (expected); \
    if (actual_value != expected_value) { \
        std::cerr << __func__ << ": expected " << expected_value \
                  << ", got " << actual_value << "\n"; \
        ++failures; \
    } \
} while (0)

static NavigationCandidate candidate(int id, int x, int y, int width, int height,
                                     bool visible = true) {
    return NavigationCandidate{id, NavigationRect{x, y, width, height}, visible};
}

static void horizontal_movement_stays_on_the_same_row() {
    const NavigationRect focused{10, 10, 30, 20};
    const std::vector<NavigationCandidate> candidates{
        candidate(1, 50, 10, 30, 20),
        candidate(2, 10, 50, 30, 20),
        candidate(3, 100, 50, 30, 20),
    };

    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(),
                                           NavigationDirection::RIGHT), 1);
}

static void vertical_movement_stays_in_the_same_column() {
    const NavigationRect focused{100, 10, 30, 20};
    const std::vector<NavigationCandidate> candidates{
        candidate(1, 30, 50, 30, 20),
        candidate(2, 100, 60, 30, 20),
        candidate(3, 140, 45, 30, 20),
    };

    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(),
                                           NavigationDirection::DOWN), 2);
}

static void every_direction_is_distinct() {
    const NavigationRect focused{100, 100, 20, 20};
    const std::vector<NavigationCandidate> candidates{
        candidate(1, 100, 50, 20, 20),
        candidate(2, 100, 150, 20, 20),
        candidate(3, 50, 100, 20, 20),
        candidate(4, 150, 100, 20, 20),
    };

    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(), NavigationDirection::UP), 1);
    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(), NavigationDirection::DOWN), 2);
    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(), NavigationDirection::LEFT), 3);
    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(), NavigationDirection::RIGHT), 4);
}

static void hidden_and_wrong_direction_candidates_are_ignored() {
    const NavigationRect focused{100, 100, 20, 20};
    const std::vector<NavigationCandidate> candidates{
        candidate(1, 100, 50, 20, 20),
        candidate(2, 100, 150, 20, 20, false),
        candidate(3, 100, 90, 20, 20),
    };

    EXPECT_EQ(select_directional_candidate(focused, candidates.data(), candidates.size(),
                                           NavigationDirection::DOWN), -1);
}

int main() {
    horizontal_movement_stays_on_the_same_row();
    vertical_movement_stays_in_the_same_column();
    every_direction_is_distinct();
    hidden_and_wrong_direction_candidates_are_ignored();

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "4 trackball navigation tests passed\n";
    return EXIT_SUCCESS;
}
