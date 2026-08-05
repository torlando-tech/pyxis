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
    enum class Kind { NONE, LINK_ESTABLISHED, LINK_CLOSED, RESPONSE, FAILED, OVERSIZED };
    struct Event {
        Kind kind = Kind::NONE;
        ExternalVector<uint8_t> data;
        std::size_t transfer_size = 0;
    };

    void begin(const std::vector<uint8_t>& link_token) {
        Guard guard(_lock);
        _sealed = false;
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
            (_event.kind == Kind::RESPONSE || _event.kind == Kind::FAILED ||
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
        if (transfer_size > MAX_WIRE_BYTES || size > MAX_WIRE_BYTES || (!data && size != 0)) {
            set_oversized(transfer_size);
            return true;
        }
        _event.kind = Kind::RESPONSE;
        _event.transfer_size = transfer_size;
        _event.data.clear();
        if (size != 0) _event.data.assign(data, data + size);
        return true;
    }

    bool publish_failed(const std::vector<uint8_t>& token) {
        Guard guard(_lock);
        if (_sealed) return false;
        if (token.empty()) return false;
        if (_request_token.empty()) _request_token = token;
        if (token != _request_token) return false;
        if (_event.kind == Kind::OVERSIZED || _event.kind == Kind::RESPONSE) return false;
        _event.kind = Kind::FAILED;
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
        if (transfer_size <= MAX_WIRE_BYTES) return true;
        set_oversized(transfer_size);
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
    void prepare() {
        Guard guard(_lock);
        reset(false);
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
        _event.data.clear();
        _event.transfer_size = transfer_size;
    }

    std::atomic_flag _lock = ATOMIC_FLAG_INIT;
    bool _sealed = false;
    std::vector<uint8_t> _link_token;
    std::vector<uint8_t> _request_token;
    Event _event;
};

} // namespace UI::LXMF::NomadNet
