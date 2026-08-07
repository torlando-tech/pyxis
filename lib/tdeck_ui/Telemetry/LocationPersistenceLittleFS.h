#ifndef PYXIS_TELEMETRY_LOCATION_PERSISTENCE_LITTLEFS_H
#define PYXIS_TELEMETRY_LOCATION_PERSISTENCE_LITTLEFS_H

#include "LocationPersistence.h"

namespace Telemetry {

// Adapter for an already-mounted LittleFS instance. The mount result must be
// supplied by the owner; this adapter never mounts, formats, or erases a
// filesystem in response to an availability failure. Custom path strings must
// remain valid for the lifetime of the adapter.
class LocationPersistenceLittleFS : public LocationPersistenceStorage {
public:
    explicit LocationPersistenceLittleFS(
        bool filesystem_available,
        const char* live_path = "/littlefs/location_state.bin",
        const char* temp_path = "/littlefs/location_state.tmp",
        const char* backup_path = "/littlefs/location_state.bak")
        : available_(filesystem_available),
          live_path_(live_path),
          temp_path_(temp_path),
          backup_path_(backup_path) {}

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
    const char* path(LocationPersistenceSlot slot) const;
    bool available_ = false;
    const char* live_path_ = nullptr;
    const char* temp_path_ = nullptr;
    const char* backup_path_ = nullptr;
};

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_PERSISTENCE_LITTLEFS_H
