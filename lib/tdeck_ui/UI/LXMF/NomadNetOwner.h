#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "NomadNetActionMailbox.h"
#include "NomadNetHistory.h"

namespace UI::LXMF::NomadNet {

class OwnerSubmissionSource {
public:
    virtual ~OwnerSubmissionSource() = default;
    virtual bool prepare_submission(uint16_t item_id, uint32_t generation,
                                    std::string& target,
                                    ExternalVector<uint8_t>& request_data,
                                    FormEncodeResult& result) = 0;
};

enum class OwnerResult : uint8_t {
    NONE,
    REQUEST,
    BACK_EMPTY,
    INVALID_SUBMISSION,
    ALLOCATION_FAILED,
    HISTORY_FULL,
};

struct OwnerCommand {
    OwnerResult result = OwnerResult::NONE;
    std::string target;
    ExternalVector<uint8_t> request_data;
    PageHistory::PendingOpen pending_history;
    int32_t restore_scroll = -1;
    uint32_t generation = 0;
    bool add_history = false;
    bool cache_bypass = false;

    OwnerCommand() = default;
    OwnerCommand(const OwnerCommand&) = delete;
    OwnerCommand& operator=(const OwnerCommand&) = delete;
    OwnerCommand(OwnerCommand&& other) noexcept { move_from(other); }
    OwnerCommand& operator=(OwnerCommand&& other) noexcept {
        if (this != &other) { clear_encoded_form(request_data); pending_history.clear(); move_from(other); }
        return *this;
    }
    ~OwnerCommand() { clear_encoded_form(request_data); }
private:
    void move_from(OwnerCommand& other) noexcept {
        result = other.result;
        target = std::move(other.target);
        request_data = std::move(other.request_data);
        pending_history = std::move(other.pending_history);
        restore_scroll = other.restore_scroll;
        generation = other.generation;
        add_history = other.add_history;
        cache_bypass = other.cache_bypass;
        other.result = OwnerResult::NONE;
        other.restore_scroll = -1;
        other.generation = 0;
        other.add_history = false;
        other.cache_bypass = false;
    }
};

// Single production owner for SUBMIT/history/Back/Reload request-byte decisions.
// UIManager supplies UI effects; the native two-process client supplies a narrow
// form source, but both execute these exact transitions and status results.
class OwnerController {
public:
    OwnerCommand service(const UserAction& action, PageHistory& history,
                         OwnerSubmissionSource& source, int32_t current_scroll);
    static bool retain_active_link(const std::string& requested_destination,
                                   const std::string& link_owner_destination,
                                   bool link_active);
    static const char* status(OwnerResult result);

private:
    uint32_t next_generation();
    uint32_t _generation = 0;
};

} // namespace UI::LXMF::NomadNet
