#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace UI::LXMF::NomadNet {

class PageHistory {
public:
    static constexpr std::size_t MAX_DEPTH = 16;

    const std::string& current() const { return _current; }
    std::size_t depth() const { return _depth; }

    void open(const std::string& address, bool add_history = true) {
        if (address == _current) return;
        if (add_history && !_current.empty()) {
            if (_depth == MAX_DEPTH) {
                for (std::size_t i = 1; i < MAX_DEPTH; ++i) _entries[i - 1] = std::move(_entries[i]);
                --_depth;
            }
            _entries[_depth++] = _current;
        }
        _current = address;
    }

    void reload() {}

    bool back() {
        if (_depth == 0) return false;
        _current = std::move(_entries[--_depth]);
        return true;
    }

    void clear() {
        _current.clear();
        _depth = 0;
    }

private:
    std::array<std::string, MAX_DEPTH> _entries{};
    std::string _current;
    std::size_t _depth = 0;
};

} // namespace UI::LXMF::NomadNet
