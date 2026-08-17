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

    class PendingOpen {
    public:
        PendingOpen() = default;
        PendingOpen(const PendingOpen&) = delete;
        PendingOpen& operator=(const PendingOpen&) = delete;
        PendingOpen(PendingOpen&& other) noexcept { move_from(other); }
        PendingOpen& operator=(PendingOpen&& other) noexcept {
            if (this != &other) { clear(); move_from(other); }
            return *this;
        }
        ~PendingOpen() { clear(); }

        bool ready() const { return _ready; }
        const std::string& address() const { return _address; }
        const ExternalVector<uint8_t>& request_data() const { return _request_data; }
        bool has_request_data() const { return _has_request_data; }
        int32_t restore_scroll() const { return _restore_scroll; }
        void clear() noexcept {
            clear_encoded_form(_request_data);
            _address.clear();
            _current_scroll = 0;
            _restore_scroll = -1;
            _revision = 0;
            _has_request_data = false;
            _add_history = true;
            _ready = false;
            _operation = Operation::NONE;
        }

    private:
        friend class PageHistory;
        enum class Operation : uint8_t { NONE, OPEN, BACK };

        void move_from(PendingOpen& other) noexcept {
            _address = std::move(other._address);
            _request_data = std::move(other._request_data);
            _current_scroll = other._current_scroll;
            _restore_scroll = other._restore_scroll;
            _revision = other._revision;
            _has_request_data = other._has_request_data;
            _add_history = other._add_history;
            _ready = other._ready;
            _operation = other._operation;
            other._current_scroll = 0;
            other._restore_scroll = -1;
            other._revision = 0;
            other._has_request_data = false;
            other._ready = false;
            other._operation = Operation::NONE;
        }

        std::string _address;
        ExternalVector<uint8_t> _request_data;
        int32_t _current_scroll = 0;
        int32_t _restore_scroll = -1;
        uint32_t _revision = 0;
        bool _has_request_data = false;
        bool _add_history = true;
        bool _ready = false;
        Operation _operation = Operation::NONE;
    };

    const std::string& current() const { return _current.address; }
    int32_t current_scroll() const { return _current.logical_scroll; }
    bool current_has_request_data() const { return _current.has_request_data; }
    const ExternalVector<uint8_t>& current_request_data() const { return _current.request_data; }
    std::size_t depth() const { return _depth; }

    bool open(const std::string& address, bool add_history = true,
              int32_t current_logical_scroll = 0,
              const uint8_t* request_data = nullptr, std::size_t request_size = 0) {
        PendingOpen pending;
        return prepare_open(address, add_history, current_logical_scroll,
                            request_data, request_size, pending) &&
               commit(std::move(pending));
    }

    bool prepare_open(const std::string& address, bool add_history,
                      int32_t current_logical_scroll,
                      const uint8_t* request_data, std::size_t request_size,
                      PendingOpen& pending) const {
        if ((!request_data && request_size != 0) || request_size > FormState::MAX_ENCODED_BYTES)
            return false;
        PendingOpen next;
        try {
            next._address = address;
            if (request_size != 0) {
                next._request_data.assign(request_data, request_data + request_size);
                next._has_request_data = true;
            }
        } catch (const std::bad_alloc&) {
            return false;
        }
        next._current_scroll = current_logical_scroll;
        next._add_history = add_history;
        next._revision = _revision;
        next._operation = PendingOpen::Operation::OPEN;
        next._ready = true;
        pending = std::move(next);
        return true;
    }

    bool prepare_back(PendingOpen& pending) const {
        if (_depth == 0) return false;
        PendingOpen next;
        const Entry& prior = _entries[_depth - 1];
        try {
            next._address = prior.address;
            if (prior.has_request_data) {
                next._request_data.assign(prior.request_data.begin(), prior.request_data.end());
                next._has_request_data = true;
            }
        } catch (const std::bad_alloc&) {
            return false;
        }
        next._restore_scroll = prior.logical_scroll;
        next._revision = _revision;
        next._operation = PendingOpen::Operation::BACK;
        next._ready = true;
        pending = std::move(next);
        return true;
    }

    bool commit(PendingOpen&& pending) noexcept {
        if (!pending._ready || pending._revision != _revision) return false;
        if (pending._operation == PendingOpen::Operation::BACK) {
            pending._ready = false;
            pending._operation = PendingOpen::Operation::NONE;
            _current = std::move(_entries[--_depth]);
            ++_revision;
            return true;
        }
        if (pending._operation != PendingOpen::Operation::OPEN) return false;

        Entry next;
        next.address = std::move(pending._address);
        next.request_data = std::move(pending._request_data);
        next.has_request_data = pending._has_request_data;
        const bool add_history = pending._add_history;
        const int32_t current_logical_scroll = pending._current_scroll;
        pending._ready = false;
        pending._has_request_data = false;
        pending._operation = PendingOpen::Operation::NONE;
        const bool same_request_data = next.has_request_data == _current.has_request_data &&
            (!next.has_request_data || next.request_data == _current.request_data);
        if (next.address == _current.address && (!add_history || same_request_data)) {
            next.logical_scroll = _current.logical_scroll;
            _current = std::move(next);
            ++_revision;
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
        ++_revision;
        return true;
    }

    void reload() {}

    bool back() {
        PendingOpen pending;
        return prepare_back(pending) && commit(std::move(pending));
    }

    void clear() {
        _current = Entry{};
        for (std::size_t i = 0; i < _depth; ++i) _entries[i] = Entry{};
        _depth = 0;
        ++_revision;
    }

private:
    struct Entry {
        Entry() = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept
            : address(std::move(other.address)), logical_scroll(other.logical_scroll),
              request_data(std::move(other.request_data)), has_request_data(other.has_request_data) {
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
    uint32_t _revision = 0;
};

} // namespace UI::LXMF::NomadNet
