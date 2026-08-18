#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include "NomadNetMemory.h"

namespace UI::LXMF::NomadNet {

// Single-slot callback-to-main-loop handoff. Reticulum callbacks only publish
// bytes and status here; the main loop is the sole consumer and LVGL owner.
class AsyncMailbox {
public:
    static constexpr std::size_t MAX_BYTES = 64 * 1024;
    static constexpr std::size_t MAX_WIRE_BYTES = MAX_BYTES + 64;
    enum class Kind { NONE, LINK_ESTABLISHED, LINK_CLOSED, PROGRESS, RESPONSE, FAILED, OVERSIZED };
    struct Event {
        Kind kind = Kind::NONE;
        ExternalVector<uint8_t> data;
        std::size_t transfer_size = 0;
        std::uint32_t generation = 0;
    };

    void begin(const std::vector<uint8_t>& link_token, std::uint32_t generation = 0) {
        Guard guard(_lock);
        _sealed = false;
        if (generation != 0) _generation = generation;
        if (_link_token == link_token &&
            (_event.kind == Kind::LINK_ESTABLISHED || _event.kind == Kind::LINK_CLOSED)) return;
        _link_token = link_token;
        _request_token.clear();
        _event = Event{};
    }

    void expect_request(const std::vector<uint8_t>& request_token) {
        Guard guard(_lock);
        _sealed = false;
        if (_request_token == request_token &&
            (_event.kind == Kind::PROGRESS || _event.kind == Kind::RESPONSE || _event.kind == Kind::FAILED ||
             _event.kind == Kind::OVERSIZED)) return;
        _request_token = request_token;
        _event = Event{};
    }

    bool publish_link(const std::vector<uint8_t>& token, bool established) {
        Guard guard(_lock);
        if (_sealed) return false;
        if (token.empty()) return false;
        if (_link_token.empty()) _link_token = token;
        if (token != _link_token) return false;
        // A remote may close immediately after delivering a response. Preserve
        // terminal request events until the main loop consumes them instead of
        // replacing the page/error with a generic LINK_CLOSED event.
        if (!established && (_event.kind == Kind::RESPONSE ||
                             _event.kind == Kind::FAILED ||
                             _event.kind == Kind::OVERSIZED)) return false;
        _event.kind = established ? Kind::LINK_ESTABLISHED : Kind::LINK_CLOSED;
        _event.generation = _generation;
        _event.data.clear();
        _event.transfer_size = 0;
        return true;
    }

    bool publish_response(const std::vector<uint8_t>& token, const uint8_t* data,
                          std::size_t size, std::size_t transfer_size) {
        Guard guard(_lock);
        if (_sealed) return false;
        if (token.empty()) return false;
        if (_request_token.empty()) _request_token = token;
        if (token != _request_token) return false;
        if (size > _max_wire_bytes || (!data && size != 0)) {
            set_oversized(size);
            return true;
        }
        _event.data.clear();
        try {
            if (size != 0) _event.data.assign(data, data + size);
        } catch (const std::bad_alloc&) {
            // Callback context must never unwind through Reticulum. Discard any
            // partial payload and publish a bounded terminal failure for the
            // accepted request token instead.
            _event.data.clear();
            _event.kind = Kind::FAILED;
            _event.generation = _generation;
            _event.transfer_size = 0;
            return true;
        }
        _event.kind = Kind::RESPONSE;
        _event.generation = _generation;
        _event.transfer_size = transfer_size;
        return true;
    }

    bool publish_failed(const std::vector<uint8_t>& token,
                        std::size_t response_size = 0) {
        Guard guard(_lock);
        if (_sealed) return false;
        if (token.empty()) return false;
        if (_request_token.empty()) _request_token = token;
        if (token != _request_token) return false;
        if (response_size > _max_wire_bytes) {
            set_oversized(response_size);
            return true;
        }
        if (_event.kind == Kind::OVERSIZED || _event.kind == Kind::RESPONSE) return false;
        _event.kind = Kind::FAILED;
        _event.generation = _generation;
        _event.data.clear();
        _event.transfer_size = 0;
        return true;
    }

    bool publish_progress(const std::vector<uint8_t>& token, std::size_t transfer_size) {
        Guard guard(_lock);
        if (_sealed) return false;
        if (token.empty()) return false;
        if (_request_token.empty()) _request_token = token;
        if (token != _request_token) return false;
        if (_event.kind == Kind::RESPONSE || _event.kind == Kind::FAILED ||
            _event.kind == Kind::OVERSIZED) return false;
        _event.kind = Kind::PROGRESS;
        _event.generation = _generation;
        _event.data.clear();
        _event.transfer_size = transfer_size;
        return true;
    }

    bool publish_oversized(const std::vector<uint8_t>& token, std::size_t response_size) {
        Guard guard(_lock);
        if (_sealed || token.empty()) return false;
        if (_request_token.empty()) _request_token = token;
        if (token != _request_token) return false;
        if (_event.kind == Kind::RESPONSE) return false;
        set_oversized(response_size);
        return true;
    }

    bool take(Event& event) {
        Guard guard(_lock);
        if (_event.kind == Kind::NONE) return false;
        event = std::move(_event);
        _event = Event{};
        return true;
    }

    void clear() {
        Guard guard(_lock);
        reset(false);
    }

    // Open an explicit pre-arm window before constructing a Link. Some
    // implementations can call back before begin() receives its token.
    void prepare(std::uint32_t generation = 0,
                 std::size_t max_wire_bytes = MAX_WIRE_BYTES) {
        Guard guard(_lock);
        reset(false);
        _generation = generation;
        _max_wire_bytes = max_wire_bytes > MAX_WIRE_BYTES
            ? MAX_WIRE_BYTES : max_wire_bytes;
    }

    // Terminal cleanup can synchronously invoke RequestReceipt's failed
    // callback. Reject all callbacks until the next operation calls prepare().
    void seal() {
        Guard guard(_lock);
        reset(true);
    }

private:
    void reset(bool sealed) {
        _link_token.clear();
        _request_token.clear();
        _event = Event{};
        _sealed = sealed;
    }

    class Guard {
    public:
        explicit Guard(std::atomic_flag& lock) : _lock(lock) {
            while (_lock.test_and_set(std::memory_order_acquire)) {}
        }
        ~Guard() { _lock.clear(std::memory_order_release); }
    private:
        std::atomic_flag& _lock;
    };

    void set_oversized(std::size_t transfer_size) {
        _event.kind = Kind::OVERSIZED;
        _event.generation = _generation;
        _event.data.clear();
        _event.transfer_size = transfer_size;
    }

    std::atomic_flag _lock = ATOMIC_FLAG_INIT;
    bool _sealed = false;
    std::vector<uint8_t> _link_token;
    std::vector<uint8_t> _request_token;
    std::uint32_t _generation = 0;
    std::size_t _max_wire_bytes = MAX_WIRE_BYTES;
    Event _event;
};

} // namespace UI::LXMF::NomadNet
