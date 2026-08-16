#pragma once

#include <cstddef>
#include <string>

namespace UI::LXMF::NomadNet {

struct Url {
    static constexpr const char* DEFAULT_PATH = "/page/index.mu";
    static constexpr std::size_t MAX_FRAGMENT_BYTES = 64;

    std::string destination_hex;
    std::string path;
    std::string fields;
    std::string fragment;
    bool has_fragment = false;

    std::string str() const {
        return destination_hex + ":" + path +
            (has_fragment ? std::string("#") + fragment : std::string()) +
            (fields.empty() ? std::string() : "`" + fields);
    }

    bool same_resource(const Url& other) const {
        return destination_hex == other.destination_hex && path == other.path &&
            fields == other.fields;
    }

    static bool parse(const std::string& input, Url& result, std::string& error,
                      const std::string& current_destination = {},
                      const std::string& current_path = {},
                      const std::string& current_fields = {});
};

inline bool should_jump_locally(const Url& current, const Url& candidate,
                                bool page_loaded, bool restoring_history) {
    return page_loaded && current.same_resource(candidate) &&
        (candidate.has_fragment || restoring_history);
}

} // namespace UI::LXMF::NomadNet
