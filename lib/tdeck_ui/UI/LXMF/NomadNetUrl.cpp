#include "NomadNetUrl.h"

#include <algorithm>
#include <cctype>

namespace UI::LXMF::NomadNet {

bool Url::parse(const std::string& input, Url& result, std::string& error,
                const std::string& current_destination) {
    error.clear();
    if (input.empty() || input.size() > 512) {
        error = "Address is empty or too long";
        return false;
    }
    for (unsigned char c : input) {
        if (c < 0x20 || c == 0x7f) {
            error = "Address contains control characters";
            return false;
        }
    }
    const auto colon = input.find(':');
    if (colon != std::string::npos && input.find(':', colon + 1) != std::string::npos) {
        error = "Address has too many separators";
        return false;
    }
    std::string destination = colon == std::string::npos ? input : input.substr(0, colon);
    std::string path = colon == std::string::npos ? DEFAULT_PATH : input.substr(colon + 1);
    if (destination.empty()) destination = current_destination;
    if (destination.size() != 32 ||
        !std::all_of(destination.begin(), destination.end(), [](unsigned char c) { return std::isxdigit(c); })) {
        error = "Destination must be exactly 32 hexadecimal characters";
        return false;
    }
    std::transform(destination.begin(), destination.end(), destination.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (path.empty()) path = DEFAULT_PATH;
    if (path.front() != '/') {
        error = "Path must begin with /";
        return false;
    }
    if (path.rfind("/file/", 0) == 0) {
        error = "Downloads are not supported in this MVP";
        return false;
    }
    result.destination_hex = std::move(destination);
    result.path = std::move(path);
    return true;
}

} // namespace UI::LXMF::NomadNet
