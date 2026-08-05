#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace UI::LXMF::NomadNet {

inline std::string compact_address(const std::string& value, std::size_t max_bytes = 42) {
    if (value.size() <= max_bytes) return value;
    if (max_bytes <= 3) return value.substr(0, max_bytes);

    static constexpr const char* ELLIPSIS = "...";
    static constexpr std::size_t ELLIPSIS_BYTES = 3;
    const auto separator = value.find(':');
    if (separator >= 12 && separator != std::string::npos && max_bytes >= 16) {
        const std::string destination = value.substr(0, 8) + ELLIPSIS +
            value.substr(separator - 4, 4);
        const std::string path = value.substr(separator + 1);
        const std::size_t path_budget = max_bytes - destination.size() - 1;
        if (path.size() <= path_budget) return destination + " " + path;
        if (path_budget <= ELLIPSIS_BYTES)
            return destination.substr(0, max_bytes - ELLIPSIS_BYTES) + ELLIPSIS;
        return destination + " " + path.substr(0, path_budget - ELLIPSIS_BYTES) + ELLIPSIS;
    }

    return value.substr(0, max_bytes - ELLIPSIS_BYTES) + ELLIPSIS;
}

} // namespace UI::LXMF::NomadNet
