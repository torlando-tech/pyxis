#pragma once

#include <cstdint>

namespace UI::LXMF::NomadNet {

// Browser-owned bounds around the pinned transport state machine. Path waiting
// must never be shorter than microReticulum's 15 second PATH_REQUEST_TIMEOUT.
// A failed cached route gets one fresh discovery attempt, then terminates.
class RequestPolicy {
public:
    static constexpr uint32_t PATH_WAIT_MS = 20000;
    static constexpr uint32_t LINK_WAIT_MS = 30000;

    enum class LinkTimeoutAction : uint8_t { REFRESH_PATH, FAIL };

    void reset() { _path_refreshes = 0; }

    LinkTimeoutAction on_link_timeout() {
        if (_path_refreshes == 0) {
            ++_path_refreshes;
            return LinkTimeoutAction::REFRESH_PATH;
        }
        return LinkTimeoutAction::FAIL;
    }

    uint8_t path_refreshes() const { return _path_refreshes; }
    static bool path_invalidation_succeeded(bool path_present_after) {
        return !path_present_after;
    }

private:
    uint8_t _path_refreshes = 0;
};

} // namespace UI::LXMF::NomadNet
