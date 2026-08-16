#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace UI::LXMF::NomadNet {

class PageHistory {
public:
    static constexpr std::size_t MAX_DEPTH = 16;

    const std::string& current() const { return _current.address; }
    int32_t current_scroll() const { return _current.logical_scroll; }
    std::size_t depth() const { return _depth; }

    void open(const std::string& address, bool add_history = true,
              int32_t current_logical_scroll = 0) {
        if (address == _current.address) return;
        if (add_history && !_current.address.empty()) {
            _current.logical_scroll = current_logical_scroll;
            if (_depth == MAX_DEPTH) {
                for (std::size_t i = 1; i < MAX_DEPTH; ++i)
                    _entries[i - 1] = std::move(_entries[i]);
                --_depth;
            }
            _entries[_depth++] = std::move(_current);
        }
        _current = Entry{address, 0};
    }

    void reload() {}

    bool back() {
        if (_depth == 0) return false;
        _current = std::move(_entries[--_depth]);
        return true;
    }

    void clear() {
        _current = Entry{};
        _depth = 0;
    }

private:
    struct Entry {
        Entry() = default;
        Entry(const std::string& value, int32_t scroll)
            : address(value), logical_scroll(scroll) {}

        std::string address;
        int32_t logical_scroll = 0;
    };

    std::array<Entry, MAX_DEPTH> _entries{};
    Entry _current;
    std::size_t _depth = 0;
};

} // namespace UI::LXMF::NomadNet
