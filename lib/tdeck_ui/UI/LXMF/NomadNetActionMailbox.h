#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace UI::LXMF::NomadNet {

enum class UserActionKind : uint8_t { OPEN, SUBMIT, SAVE, IDENTIFY, BACK, HOME };

struct UserAction {
    static constexpr std::size_t MAX_TARGET_BYTES = 511;
    UserActionKind kind = UserActionKind::BACK;
    std::array<char, MAX_TARGET_BYTES + 1> target_bytes{};
    std::size_t target_length = 0;
    uint16_t item_id = 0;
    uint32_t generation = 0;

    std::string target() const { return std::string(target_bytes.data(), target_length); }
};

// LVGL callbacks only publish bounded data here. The UIManager owner loop pops
// actions and performs Reticulum, path-table, filesystem, and route work.
class ActionMailbox {
public:
    static constexpr std::size_t CAPACITY = 4;

    bool publish(UserActionKind kind, const std::string& target) {
        if (target.size() > UserAction::MAX_TARGET_BYTES) return false;
        Guard guard(_lock);
        if (kind == UserActionKind::BACK || kind == UserActionKind::HOME) {
            std::array<UserAction, CAPACITY> retained{};
            std::size_t retained_count = 0;
            for (std::size_t i = 0; i < _count && retained_count < CAPACITY; ++i) {
                const UserAction& pending = _queue[(_head + i) % CAPACITY];
                if (pending.kind == UserActionKind::SAVE ||
                    pending.kind == UserActionKind::IDENTIFY)
                    retained[retained_count++] = pending;
            }
            _queue = retained;
            _head = 0;
            _count = retained_count;
            _terminal_kind = kind;
            _terminal_pending = true;
            return true;
        } else if (_terminal_pending && kind == UserActionKind::OPEN) {
            return false;
        }
        if (kind == UserActionKind::SAVE) {
            for (std::size_t i = 0; i < _count; ++i) {
                const UserAction& pending = _queue[(_head + i) % CAPACITY];
                if (pending.kind == UserActionKind::SAVE &&
                    pending.target_length == target.size() &&
                    std::memcmp(pending.target_bytes.data(), target.data(), target.size()) == 0)
                    return true;
            }
        }
        if (_count == CAPACITY) return false;
        UserAction& slot = _queue[(_head + _count) % CAPACITY];
        slot.kind = kind;
        slot.target_length = target.size();
        if (!target.empty()) std::memcpy(slot.target_bytes.data(), target.data(), target.size());
        slot.target_bytes[target.size()] = '\0';
        ++_count;
        return true;
    }

    bool publish_submit(uint16_t link_id, uint32_t generation) {
        Guard guard(_lock);
        if (_terminal_pending) return false;
        for (std::size_t i = 0; i < _count; ++i) {
            const UserAction& pending = _queue[(_head + i) % CAPACITY];
            if (pending.kind != UserActionKind::SUBMIT) continue;
            return pending.item_id == link_id && pending.generation == generation;
        }
        if (_count == CAPACITY) return false;
        UserAction& slot = _queue[(_head + _count) % CAPACITY];
        slot = UserAction{};
        slot.kind = UserActionKind::SUBMIT;
        slot.item_id = link_id;
        slot.generation = generation;
        ++_count;
        return true;
    }

    bool publish_identify(const std::string& destination, bool identified) {
        if (destination.size() > UserAction::MAX_TARGET_BYTES) return false;
        Guard guard(_lock);
        if (_terminal_pending) return false;
        for (std::size_t i = 0; i < _count; ++i) {
            UserAction& pending = _queue[(_head + i) % CAPACITY];
            if (pending.kind == UserActionKind::IDENTIFY &&
                pending.target_length == destination.size() &&
                std::memcmp(pending.target_bytes.data(), destination.data(), destination.size()) == 0) {
                pending.item_id = identified ? 1 : 0;
                return true;
            }
        }
        if (_count == CAPACITY) return false;
        UserAction& slot = _queue[(_head + _count) % CAPACITY];
        slot = UserAction{};
        slot.kind = UserActionKind::IDENTIFY;
        slot.target_length = destination.size();
        if (!destination.empty())
            std::memcpy(slot.target_bytes.data(), destination.data(), destination.size());
        slot.target_bytes[destination.size()] = '\0';
        slot.item_id = identified ? 1 : 0;
        ++_count;
        return true;
    }

    bool pop(UserAction& action) {
        Guard guard(_lock);
        if (_count != 0) {
            action = _queue[_head];
            _head = (_head + 1) % CAPACITY;
            --_count;
            return true;
        }
        if (!_terminal_pending) return false;
        action = UserAction{};
        action.kind = _terminal_kind;
        _terminal_pending = false;
        return true;
    }

    bool terminal_pending() {
        Guard guard(_lock);
        return _terminal_pending;
    }

    void clear() {
        Guard guard(_lock);
        _head = 0;
        _count = 0;
        _terminal_pending = false;
    }

private:
    class Guard {
    public:
        explicit Guard(std::atomic_flag& lock) : _lock(lock) {
            while (_lock.test_and_set(std::memory_order_acquire)) {}
        }
        ~Guard() { _lock.clear(std::memory_order_release); }
    private:
        std::atomic_flag& _lock;
    };

    std::array<UserAction, CAPACITY> _queue{};
    std::size_t _head = 0;
    std::size_t _count = 0;
    UserActionKind _terminal_kind = UserActionKind::BACK;
    bool _terminal_pending = false;
    std::atomic_flag _lock = ATOMIC_FLAG_INIT;
};

} // namespace UI::LXMF::NomadNet
