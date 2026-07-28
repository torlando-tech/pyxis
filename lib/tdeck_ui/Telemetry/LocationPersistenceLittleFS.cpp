#include "LocationPersistenceLittleFS.h"

#include <FS.h>
#include <LittleFS.h>

#include <cstddef>
#include <cstdint>

namespace Telemetry {

const char* LocationPersistenceLittleFS::path(LocationPersistenceSlot slot) {
    switch (slot) {
        case LocationPersistenceSlot::LIVE:
            return "/location_state.bin";
        case LocationPersistenceSlot::TEMP:
            return "/location_state.tmp";
        case LocationPersistenceSlot::BACKUP:
            return "/location_state.bak";
    }
    return "";
}

bool LocationPersistenceLittleFS::stat(
    LocationPersistenceSlot slot,
    bool& exists) {
    if (!available_) return false;
    exists = LittleFS.exists(path(slot));
    return true;
}

bool LocationPersistenceLittleFS::read(
    LocationPersistenceSlot slot,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& size) {
    size = 0;
    if (!available_ || output == nullptr) return false;
    File file = LittleFS.open(path(slot), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return false;
    }
    const std::size_t file_size = file.size();
    if (file_size > capacity) {
        file.close();
        return false;
    }
    const std::size_t read_size = file.read(output, file_size);
    file.close();
    if (read_size != file_size) return false;
    size = read_size;
    return true;
}

bool LocationPersistenceLittleFS::write(
    LocationPersistenceSlot slot,
    const uint8_t* data,
    std::size_t size) {
    if (!available_ || data == nullptr) return false;
    File file = LittleFS.open(path(slot), FILE_WRITE);
    if (!file || file.isDirectory()) {
        if (file) file.close();
        return false;
    }
    const std::size_t written = file.write(data, size);
    file.flush();
    const bool successful =
        written == size && file.getWriteError() == 0;
    file.close();
    return successful;
}

bool LocationPersistenceLittleFS::remove(LocationPersistenceSlot slot) {
    if (!available_) return false;
    return LittleFS.remove(path(slot));
}

bool LocationPersistenceLittleFS::rename(
    LocationPersistenceSlot from,
    LocationPersistenceSlot to) {
    if (!available_) return false;
    return LittleFS.rename(path(from), path(to));
}

}  // namespace Telemetry
