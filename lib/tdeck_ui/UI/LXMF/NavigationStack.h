#pragma once

#include <array>
#include <cstddef>

namespace UI::LXMF {

enum class Route {
    HOME,
    MESSAGES,
    MAP,
    LOCATION_SHARING,
    CHAT,
    COMPOSE,
    NETWORK,
    ANNOUNCES,
    STATUS,
    RADIO_ACTIVITY,
    QR,
    PROPAGATION_NODES,
    NOMADNET,
    SETTINGS,
    CALL,
};

class NavigationStack {
public:
    static constexpr std::size_t MAX_DEPTH = 12;

    Route current() const { return _current; }
    std::size_t depth() const { return _depth; }

    void navigate(Route route) {
        if (route == _current) return;
        if (_depth == MAX_DEPTH) {
            // Preserve the root entry so a saturated stack still unwinds Home.
            for (std::size_t i = 2; i < MAX_DEPTH; ++i) _stack[i - 1] = _stack[i];
            --_depth;
        }
        _stack[_depth++] = _current;
        _current = route;
    }

    bool back() {
        if (_depth == 0) return false;
        _current = _stack[--_depth];
        return true;
    }

    void home() {
        _depth = 0;
        _current = Route::HOME;
    }

    // Replace is for restoring a route after a transient call without adding
    // an artificial history entry.
    void replace(Route route) { _current = route; }

private:
    Route _current = Route::HOME;
    std::array<Route, MAX_DEPTH> _stack{};
    std::size_t _depth = 0;
};

} // namespace UI::LXMF
