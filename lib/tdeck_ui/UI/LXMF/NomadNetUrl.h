#pragma once

#include <string>

namespace UI::LXMF::NomadNet {

struct Url {
    static constexpr const char* DEFAULT_PATH = "/page/index.mu";
    std::string destination_hex;
    std::string path;

    std::string str() const { return destination_hex + ":" + path; }

    static bool parse(const std::string& input, Url& result, std::string& error,
                      const std::string& current_destination = {});
};

} // namespace UI::LXMF::NomadNet
