#include "NomadNetOwner.h"

#include <new>
#include <utility>

namespace UI::LXMF::NomadNet {

uint32_t OwnerController::next_generation() {
    ++_generation;
    if (_generation == 0) ++_generation;
    return _generation;
}

OwnerCommand OwnerController::service(const UserAction& action, PageHistory& history,
                                      OwnerSubmissionSource& source,
                                      int32_t current_scroll) {
    OwnerCommand command;
    command.generation = next_generation();
    try {
        if (action.kind == UserActionKind::SUBMIT) {
            FormEncodeResult encoded = FormEncodeResult::INVALID_STATE;
            if (!source.prepare_submission(action.item_id, action.generation,
                                           command.target, command.request_data, encoded)) {
                command.result = encoded == FormEncodeResult::ALLOCATION_FAILED
                    ? OwnerResult::ALLOCATION_FAILED : OwnerResult::INVALID_SUBMISSION;
                return command;
            }
            if (!history.prepare_open(command.target, true, current_scroll,
                              command.request_data.data(), command.request_data.size(),
                              command.pending_history)) {
                command.result = OwnerResult::HISTORY_FULL;
                return command;
            }
            command.result = OwnerResult::REQUEST;
            return command;
        }
        if (action.kind == UserActionKind::RELOAD) {
            if (history.current().empty()) {
                command.result = OwnerResult::BACK_EMPTY;
                return command;
            }
            command.target = history.current();
            command.restore_scroll = -1;
            command.cache_bypass = true;
        } else if (action.kind == UserActionKind::BACK) {
            if (history.depth() == 0) {
                command.result = OwnerResult::BACK_EMPTY;
                return command;
            }
            if (!history.prepare_back(command.pending_history)) {
                command.result = OwnerResult::ALLOCATION_FAILED;
                return command;
            }
            command.target = command.pending_history.address();
            command.restore_scroll = command.pending_history.restore_scroll();
        } else {
            command.result = OwnerResult::NONE;
            return command;
        }
        const bool pending_back = command.pending_history.ready();
        const bool has_saved = pending_back ? command.pending_history.has_request_data()
                                            : history.current_has_request_data();
        if (has_saved) {
            const auto& saved = pending_back ? command.pending_history.request_data()
                                             : history.current_request_data();
            command.request_data.assign(saved.begin(), saved.end());
        }
        command.result = OwnerResult::REQUEST;
        return command;
    } catch (const std::bad_alloc&) {
        command.result = OwnerResult::ALLOCATION_FAILED;
        command.target.clear();
        clear_encoded_form(command.request_data);
        command.pending_history.clear();
        return command;
    }
}

bool OwnerController::retain_active_link(const std::string& requested_destination,
                                         const std::string& link_owner_destination,
                                         bool link_active) {
    return link_active && !requested_destination.empty() &&
           requested_destination == link_owner_destination;
}

const char* OwnerController::status(OwnerResult result) {
    switch (result) {
        case OwnerResult::INVALID_SUBMISSION: return "Form changed before submission";
        case OwnerResult::ALLOCATION_FAILED: return "Form submission exceeds available memory";
        case OwnerResult::HISTORY_FULL: return "Form history exceeds available memory";
        case OwnerResult::BACK_EMPTY: return "Enter a NomadNet address";
        case OwnerResult::NONE:
        case OwnerResult::REQUEST: return "Requesting page...";
    }
    return "NomadNet owner error";
}

} // namespace UI::LXMF::NomadNet
