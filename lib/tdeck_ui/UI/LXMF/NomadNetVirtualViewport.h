#pragma once

#include <algorithm>
#include <cstdint>

namespace UI::LXMF::NomadNet {

class VirtualViewport {
public:
    static int32_t logical_from_physical(int32_t physical_scroll,
                                         int32_t logical_height,
                                         int32_t viewport_height,
                                         int32_t physical_extent) {
        const int32_t logical_max = std::max<int32_t>(0, logical_height - viewport_height);
        const int32_t physical_max = std::max<int32_t>(0, physical_extent - viewport_height);
        if (logical_max == 0 || physical_max == 0) return 0;
        const int32_t clamped = std::max<int32_t>(0, std::min(physical_scroll, physical_max));
        if (logical_max == physical_max) return clamped;
        return static_cast<int32_t>((static_cast<int64_t>(clamped) * logical_max) / physical_max);
    }

    static int32_t physical_from_logical(int32_t logical_scroll,
                                         int32_t logical_height,
                                         int32_t viewport_height,
                                         int32_t physical_extent) {
        const int32_t logical_max = std::max<int32_t>(0, logical_height - viewport_height);
        const int32_t physical_max = std::max<int32_t>(0, physical_extent - viewport_height);
        if (logical_max == 0 || physical_max == 0) return 0;
        const int32_t clamped = std::max<int32_t>(0, std::min(logical_scroll, logical_max));
        if (logical_max == physical_max) return clamped;
        return static_cast<int32_t>((static_cast<int64_t>(clamped) * physical_max) / logical_max);
    }

    static int32_t window_top(int32_t logical_scroll, int32_t viewport_height) {
        return std::max<int32_t>(0, logical_scroll - viewport_height * 2);
    }

    static int32_t window_bottom(int32_t logical_scroll,
                                 int32_t viewport_height,
                                 int32_t logical_height) {
        return std::min<int32_t>(logical_height, logical_scroll + viewport_height * 3);
    }

    static bool can_coalesce(uint16_t existing_run,
                             uint16_t existing_offset,
                             uint16_t existing_length,
                             uint16_t next_run,
                             uint16_t next_offset) {
        return existing_run == next_run &&
            static_cast<uint32_t>(existing_offset) + existing_length == next_offset;
    }
};

} // namespace UI::LXMF::NomadNet
