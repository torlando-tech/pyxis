#ifndef PYXIS_TELEMETRY_LOCATION_PERSISTENCE_LITTLEFS_H
#define PYXIS_TELEMETRY_LOCATION_PERSISTENCE_LITTLEFS_H

#include "LocationPersistence.h"

namespace Telemetry {

// Adapter for an already-mounted LittleFS instance. The mount result must be
// supplied by the owner; this adapter never mounts, formats, or erases a
// filesystem in response to an availability failure.
class LocationPersistenceLittleFS : public LocationPersistenceStorage {
public:
    explicit LocationPersistenceLittleFS(bool filesystem_available)
        : available_(filesystem_available) {}

    void setAvailable(bool available) { available_ = available; }
    bool available() const override { return available_; }
    bool stat(LocationPersistenceSlot slot, bool& exists) override;
    bool read(LocationPersistenceSlot slot, uint8_t* output,
              std::size_t capacity, std::size_t& size) override;
    bool write(LocationPersistenceSlot slot, const uint8_t* data,
               std::size_t size) override;
    bool remove(LocationPersistenceSlot slot) override;
    bool rename(LocationPersistenceSlot from,
                LocationPersistenceSlot to) override;

private:
    static const char* path(LocationPersistenceSlot slot);
    bool available_ = false;
};

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_PERSISTENCE_LITTLEFS_H
