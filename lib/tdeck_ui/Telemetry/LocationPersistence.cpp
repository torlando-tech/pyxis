#include "LocationPersistence.h"

#include <cstddef>
#include <cstdint>

namespace Telemetry {

TransactionalLocationPersistence::CandidateResult
TransactionalLocationPersistence::readCandidate(
    LocationPersistenceSlot slot,
    std::size_t& size) {
    size = 0;
    bool exists = false;
    if (!storage_.stat(slot, exists)) return CandidateResult::IO_ERROR;
    if (!exists) return CandidateResult::ABSENT;
    if (!storage_.read(slot, buffer_, sizeof(buffer_), size)) {
        return CandidateResult::IO_ERROR;
    }
    if (validateLocationStateRecord(buffer_, size) !=
        LocationStateRecordResult::OK) {
        return CandidateResult::INVALID;
    }
    return CandidateResult::VALID;
}

bool TransactionalLocationPersistence::removeIfPresent(
    LocationPersistenceSlot slot) {
    bool exists = false;
    if (!storage_.stat(slot, exists)) return false;
    return !exists || storage_.remove(slot);
}

LocationPersistenceResult TransactionalLocationPersistence::save(
    const LocationStateSnapshot& state) {
    if (!storage_.available()) return LocationPersistenceResult::UNAVAILABLE;

    std::size_t encoded_size = 0;
    if (encodeLocationStateRecord(
            state, buffer_, sizeof(buffer_), encoded_size) !=
        LocationStateRecordResult::OK) {
        return LocationPersistenceResult::ENCODE_ERROR;
    }
    if (!removeIfPresent(LocationPersistenceSlot::TEMP)) {
        return LocationPersistenceResult::IO_ERROR;
    }
    if (!storage_.write(LocationPersistenceSlot::TEMP, buffer_, encoded_size)) {
        return LocationPersistenceResult::IO_ERROR;
    }

    std::size_t candidate_size = 0;
    const CandidateResult temp =
        readCandidate(LocationPersistenceSlot::TEMP, candidate_size);
    if (temp == CandidateResult::IO_ERROR) {
        return LocationPersistenceResult::IO_ERROR;
    }
    if (temp != CandidateResult::VALID) {
        return LocationPersistenceResult::INVALID_STATE;
    }

    const CandidateResult live =
        readCandidate(LocationPersistenceSlot::LIVE, candidate_size);
    if (live == CandidateResult::IO_ERROR) {
        return LocationPersistenceResult::IO_ERROR;
    }
    if (live == CandidateResult::VALID) {
        if (!removeIfPresent(LocationPersistenceSlot::BACKUP)) {
            return LocationPersistenceResult::IO_ERROR;
        }
        if (!storage_.rename(LocationPersistenceSlot::LIVE,
                             LocationPersistenceSlot::BACKUP)) {
            return LocationPersistenceResult::IO_ERROR;
        }
    } else if (live == CandidateResult::INVALID) {
        if (!storage_.remove(LocationPersistenceSlot::LIVE)) {
            return LocationPersistenceResult::IO_ERROR;
        }
    }

    if (!storage_.rename(LocationPersistenceSlot::TEMP,
                         LocationPersistenceSlot::LIVE)) {
        return LocationPersistenceResult::IO_ERROR;
    }
    const CandidateResult promoted =
        readCandidate(LocationPersistenceSlot::LIVE, candidate_size);
    if (promoted == CandidateResult::IO_ERROR) {
        return LocationPersistenceResult::IO_ERROR;
    }
    if (promoted != CandidateResult::VALID) {
        return LocationPersistenceResult::INVALID_STATE;
    }
    return LocationPersistenceResult::SAVED;
}

void TransactionalLocationPersistence::repairFromTemp() {
    bool live_exists = false;
    if (!storage_.stat(LocationPersistenceSlot::LIVE, live_exists)) return;
    if (live_exists && !storage_.remove(LocationPersistenceSlot::LIVE)) return;
    storage_.rename(LocationPersistenceSlot::TEMP,
                    LocationPersistenceSlot::LIVE);
}

void TransactionalLocationPersistence::repairFromBackup(std::size_t size) {
    if (!removeIfPresent(LocationPersistenceSlot::TEMP)) return;
    if (!storage_.write(LocationPersistenceSlot::TEMP, buffer_, size)) return;

    std::size_t verified_size = 0;
    if (readCandidate(LocationPersistenceSlot::TEMP, verified_size) !=
        CandidateResult::VALID) {
        return;
    }
    bool live_exists = false;
    if (!storage_.stat(LocationPersistenceSlot::LIVE, live_exists)) return;
    if (live_exists && !storage_.remove(LocationPersistenceSlot::LIVE)) return;
    storage_.rename(LocationPersistenceSlot::TEMP,
                    LocationPersistenceSlot::LIVE);
}

LocationPersistenceResult TransactionalLocationPersistence::load(
    LocationStateSnapshot& output) {
    if (!storage_.available()) return LocationPersistenceResult::UNAVAILABLE;

    bool saw_io_error = false;
    bool saw_invalid = false;
    std::size_t size = 0;
    CandidateResult result = readCandidate(LocationPersistenceSlot::LIVE, size);
    if (result == CandidateResult::VALID) {
        return decodeLocationStateRecord(buffer_, size, output) ==
                       LocationStateRecordResult::OK
                   ? LocationPersistenceResult::LOADED_LIVE
                   : LocationPersistenceResult::INVALID_STATE;
    }
    saw_io_error = result == CandidateResult::IO_ERROR;
    saw_invalid = result == CandidateResult::INVALID;

    result = readCandidate(LocationPersistenceSlot::TEMP, size);
    if (result == CandidateResult::VALID) {
        if (decodeLocationStateRecord(buffer_, size, output) !=
            LocationStateRecordResult::OK) {
            return LocationPersistenceResult::INVALID_STATE;
        }
        repairFromTemp();
        return LocationPersistenceResult::RECOVERED_TEMP;
    }
    saw_io_error = saw_io_error || result == CandidateResult::IO_ERROR;
    saw_invalid = saw_invalid || result == CandidateResult::INVALID;

    result = readCandidate(LocationPersistenceSlot::BACKUP, size);
    if (result == CandidateResult::VALID) {
        if (decodeLocationStateRecord(buffer_, size, output) !=
            LocationStateRecordResult::OK) {
            return LocationPersistenceResult::INVALID_STATE;
        }
        repairFromBackup(size);
        return LocationPersistenceResult::RECOVERED_BACKUP;
    }
    saw_io_error = saw_io_error || result == CandidateResult::IO_ERROR;
    saw_invalid = saw_invalid || result == CandidateResult::INVALID;

    if (saw_io_error) return LocationPersistenceResult::IO_ERROR;
    if (saw_invalid) return LocationPersistenceResult::INVALID_STATE;
    return LocationPersistenceResult::NOT_FOUND;
}

}  // namespace Telemetry
