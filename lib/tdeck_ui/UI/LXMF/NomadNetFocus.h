#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace UI::LXMF::NomadNet {

struct FocusSpan {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t run_index = 0;

    FocusSpan() = default;
    constexpr FocusSpan(int16_t x_value, int16_t y_value, int16_t width_value,
                        int16_t height_value, uint16_t run_index_value)
        : x(x_value), y(y_value), width(width_value), height(height_value),
          run_index(run_index_value) {}
};

template <typename FragmentRange, typename Callback>
void for_each_focus_span(const FragmentRange& fragments, int16_t selected_link,
                         Callback callback) {
    std::size_t index = 0;
    while (index < fragments.size()) {
        const auto& first = fragments[index];
        if (first.link_index != selected_link) {
            ++index;
            continue;
        }

        int32_t left = first.x;
        int32_t top = first.y;
        int32_t right = left + std::max<int16_t>(first.width, 1);
        int32_t bottom = top + std::max<int16_t>(first.height, 1);
        std::size_t end = index + 1;
        while (end < fragments.size()) {
            const auto& fragment = fragments[end];
            if (fragment.link_index != selected_link || fragment.y != first.y ||
                fragment.run_index != first.run_index) break;
            left = std::min<int32_t>(left, fragment.x);
            right = std::max<int32_t>(right,
                static_cast<int32_t>(fragment.x) + std::max<int16_t>(fragment.width, 1));
            bottom = std::max<int32_t>(bottom,
                static_cast<int32_t>(fragment.y) + std::max<int16_t>(fragment.height, 1));
            ++end;
        }

        callback(FocusSpan{
            static_cast<int16_t>(left),
            static_cast<int16_t>(top),
            static_cast<int16_t>(right - left),
            static_cast<int16_t>(bottom - top),
            first.run_index,
        });
        index = end;
    }
}

} // namespace UI::LXMF::NomadNet
