#include "LocationPersistenceLittleFS.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace Telemetry {

const char* LocationPersistenceLittleFS::path(
    LocationPersistenceSlot slot) const {
    switch (slot) {
        case LocationPersistenceSlot::LIVE:
            return live_path_;
        case LocationPersistenceSlot::TEMP:
            return temp_path_;
        case LocationPersistenceSlot::BACKUP:
            return backup_path_;
    }
    return nullptr;
}

bool LocationPersistenceLittleFS::stat(
    LocationPersistenceSlot slot,
    bool& exists) {
    exists = false;
    const char* file_path = path(slot);
    if (!available_ || file_path == nullptr) return false;
    struct ::stat information {};
    errno = 0;
    if (::stat(file_path, &information) == 0) {
        exists = true;
        return true;
    }
    if (errno == ENOENT) return true;
    return false;
}

bool LocationPersistenceLittleFS::read(
    LocationPersistenceSlot slot,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& size) {
    size = 0;
    const char* file_path = path(slot);
    if (!available_ || file_path == nullptr || output == nullptr) return false;

    std::FILE* file = std::fopen(file_path, "rb");
    if (file == nullptr) return false;
    struct ::stat information {};
    bool successful = ::fstat(::fileno(file), &information) == 0 &&
                      S_ISREG(information.st_mode) &&
                      information.st_size >= 0 &&
                      static_cast<uint64_t>(information.st_size) <= capacity;
    std::size_t expected = 0;
    std::size_t read_size = 0;
    if (successful) {
        expected = static_cast<std::size_t>(information.st_size);
        read_size = std::fread(output, 1, expected, file);
        successful = read_size == expected && std::ferror(file) == 0;
    }
    if (successful) {
        const int trailing = std::fgetc(file);
        successful = trailing == EOF && std::ferror(file) == 0;
    }
    if (std::fclose(file) != 0) successful = false;
    if (!successful) return false;
    size = read_size;
    return true;
}

bool LocationPersistenceLittleFS::write(
    LocationPersistenceSlot slot,
    const uint8_t* data,
    std::size_t size) {
    const char* file_path = path(slot);
    if (!available_ || file_path == nullptr || data == nullptr) return false;

    std::FILE* file = std::fopen(file_path, "wb");
    if (file == nullptr) return false;
    bool successful = std::fwrite(data, 1, size, file) == size;
    if (std::fflush(file) != 0) successful = false;
    if (::fsync(::fileno(file)) != 0) successful = false;
    if (std::fclose(file) != 0) successful = false;
    return successful;
}

bool LocationPersistenceLittleFS::remove(LocationPersistenceSlot slot) {
    const char* file_path = path(slot);
    return available_ && file_path != nullptr &&
           std::remove(file_path) == 0;
}

bool LocationPersistenceLittleFS::rename(
    LocationPersistenceSlot from,
    LocationPersistenceSlot to) {
    const char* from_path = path(from);
    const char* to_path = path(to);
    return available_ && from_path != nullptr && to_path != nullptr &&
           std::rename(from_path, to_path) == 0;
}

}  // namespace Telemetry
