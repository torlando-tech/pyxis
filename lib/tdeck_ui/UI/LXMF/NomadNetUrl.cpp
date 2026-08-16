#include "NomadNetUrl.h"

#include <algorithm>
#include <cctype>

namespace UI::LXMF::NomadNet {
namespace {

bool valid_fragment(const std::string& fragment) {
    if (fragment.size() > Url::MAX_FRAGMENT_BYTES) return false;
    return std::all_of(fragment.begin(), fragment.end(), [](unsigned char c) {
        return c < 0x80 && (std::isalnum(c) != 0 || c == '_' || c == '-');
    });
}

} // namespace

bool Url::parse(const std::string& input, Url& result, std::string& error,
                const std::string& current_destination,
                const std::string& current_path,
                const std::string& current_fields) {
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

    const auto fields_separator = input.find('`');
    if (fields_separator != std::string::npos &&
        input.find('`', fields_separator + 1) != std::string::npos) {
        error = "Address has too many field separators";
        return false;
    }
    std::string address = fields_separator == std::string::npos
        ? input : input.substr(0, fields_separator);
    std::string fields = fields_separator == std::string::npos
        ? std::string() : input.substr(fields_separator + 1);

    const auto fragment_separator = address.find('#');
    const bool has_fragment = fragment_separator != std::string::npos;
    std::string fragment;
    if (has_fragment) {
        fragment = address.substr(fragment_separator + 1);
        address.resize(fragment_separator);
        if (!valid_fragment(fragment)) {
            error = "Fragment must contain only letters, digits, _ or -";
            return false;
        }
    }

    const bool fragment_only = has_fragment && address.empty();
    if (fragment_only && fields_separator != std::string::npos) {
        error = "Fragment-only links cannot replace request fields";
        return false;
    }
    if (fragment_only) fields = current_fields;

    const auto colon = address.find(':');
    if (colon != std::string::npos && address.find(':', colon + 1) != std::string::npos) {
        error = "Address has too many separators";
        return false;
    }
    std::string destination = colon == std::string::npos ? address : address.substr(0, colon);
    std::string path = colon == std::string::npos ? DEFAULT_PATH : address.substr(colon + 1);
    if (fragment_only) path = current_path.empty() ? DEFAULT_PATH : current_path;
    if (destination.empty()) destination = current_destination;
    if (destination.size() != 32 ||
        !std::all_of(destination.begin(), destination.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
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
    result.fields = std::move(fields);
    result.fragment = std::move(fragment);
    result.has_fragment = has_fragment;
    return true;
}

} // namespace UI::LXMF::NomadNet
