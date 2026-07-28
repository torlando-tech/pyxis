#ifndef PYXIS_TELEMETRY_LOCATION_PERSISTENCE_H
#define PYXIS_TELEMETRY_LOCATION_PERSISTENCE_H

#include <cstddef>
#include <cstdint>

#include "LocationStateRecord.h"

namespace Telemetry {

enum class LocationPersistenceSlot : uint8_t {
    LIVE = 0,
    TEMP = 1,
    BACKUP = 2,
};

class LocationPersistenceStorage {
public:
    virtual ~LocationPersistenceStorage() {}
    virtual bool available() const = 0;
    virtual bool stat(LocationPersistenceSlot slot, bool& exists) = 0;
    virtual bool read(LocationPersistenceSlot slot, uint8_t* output,
                      std::size_t capacity, std::size_t& size) = 0;
    virtual bool write(LocationPersistenceSlot slot, const uint8_t* data,
                       std::size_t size) = 0;
    virtual bool remove(LocationPersistenceSlot slot) = 0;
    virtual bool rename(LocationPersistenceSlot from,
                        LocationPersistenceSlot to) = 0;
};

enum class LocationPersistenceResult : uint8_t {
    SAVED,
    LOADED_LIVE,
    RECOVERED_TEMP,
    RECOVERED_BACKUP,
    NOT_FOUND,
    UNAVAILABLE,
    IO_ERROR,
    INVALID_STATE,
    ENCODE_ERROR,
};

// This object owns a 3,860-byte I/O buffer. Instantiate it in static/durable
// storage on embedded targets; do not place it on a constrained task stack.
class TransactionalLocationPersistence {
public:
    explicit TransactionalLocationPersistence(LocationPersistenceStorage& storage)
        : storage_(storage) {}

    LocationPersistenceResult save(const LocationStateSnapshot& state);
    LocationPersistenceResult load(LocationStateSnapshot& output);

private:
    enum class CandidateResult : uint8_t {
        VALID,
        ABSENT,
        IO_ERROR,
        INVALID,
    };

    CandidateResult readCandidate(LocationPersistenceSlot slot,
                                  std::size_t& size);
    bool removeIfPresent(LocationPersistenceSlot slot);
    void repairFromTemp();
    void repairFromBackup(std::size_t size);

    LocationPersistenceStorage& storage_;
    uint8_t buffer_[MAX_LOCATION_STATE_RECORD_BYTES]{};
};

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_PERSISTENCE_H
