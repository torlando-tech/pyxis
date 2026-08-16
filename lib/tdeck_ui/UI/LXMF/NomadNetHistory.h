#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

#include "NomadNetForm.h"

namespace UI::LXMF::NomadNet {

class PageHistory {
public:
    static constexpr std::size_t MAX_DEPTH = 16;

    const std::string& current() const { return _current.address; }
    int32_t current_scroll() const { return _current.logical_scroll; }
    bool current_has_request_data() const { return _current.has_request_data; }
    const ExternalVector<uint8_t>& current_request_data() const { return _current.request_data; }
    std::size_t depth() const { return _depth; }

    bool open(const std::string& address, bool add_history = true,
              int32_t current_logical_scroll = 0,
              const uint8_t* request_data = nullptr, std::size_t request_size = 0) {
        if ((!request_data && request_size != 0) || request_size > FormState::MAX_ENCODED_BYTES)
            return false;
        Entry next;
        try {
            next.address = address;
            if (request_size != 0) {
                next.request_data.assign(request_data, request_data + request_size);
                next.has_request_data = true;
            }
        } catch (const std::bad_alloc&) {
            return false;
        }
        const bool next_has_request_data = next.has_request_data;
        const bool same_request_data = next_has_request_data == _current.has_request_data &&
            (!next_has_request_data || next.request_data == _current.request_data);
        if (address == _current.address && (!add_history || same_request_data)) {
            next.logical_scroll = _current.logical_scroll;
            _current = std::move(next);
            return true;
        }
        if (add_history && !_current.address.empty()) {
            _current.logical_scroll = current_logical_scroll;
            if (_depth == MAX_DEPTH) {
                for (std::size_t i = 1; i < MAX_DEPTH; ++i)
                    _entries[i - 1] = std::move(_entries[i]);
                --_depth;
            }
            _entries[_depth++] = std::move(_current);
        }
        _current = std::move(next);
        return true;
    }

    void reload() {}

    bool back() {
        if (_depth == 0) return false;
        _current = std::move(_entries[--_depth]);
        return true;
    }

    void clear() {
        _current = Entry{};
        for (std::size_t i = 0; i < _depth; ++i) _entries[i] = Entry{};
        _depth = 0;
    }

private:
    struct Entry {
        Entry() = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept
            : address(std::move(other.address)), logical_scroll(other.logical_scroll),
              request_data(std::move(other.request_data)),
              has_request_data(other.has_request_data) {
            other.logical_scroll = 0;
            other.has_request_data = false;
        }
        Entry& operator=(Entry&& other) noexcept {
            if (this == &other) return *this;
            clear_encoded_form(request_data);
            address = std::move(other.address);
            logical_scroll = other.logical_scroll;
            request_data = std::move(other.request_data);
            has_request_data = other.has_request_data;
            other.logical_scroll = 0;
            other.has_request_data = false;
            return *this;
        }
        ~Entry() { clear_encoded_form(request_data); }

        std::string address;
        int32_t logical_scroll = 0;
        ExternalVector<uint8_t> request_data;
        bool has_request_data = false;
    };

    std::array<Entry, MAX_DEPTH> _entries{};
    Entry _current;
    std::size_t _depth = 0;
};

} // namespace UI::LXMF::NomadNet
