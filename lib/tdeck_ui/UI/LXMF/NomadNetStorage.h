// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT
#ifndef UI_LXMF_NOMADNET_STORAGE_H
#define UI_LXMF_NOMADNET_STORAGE_H
#include <cstddef>
#include <cstdint>
namespace UI { namespace LXMF { namespace NomadNet {
enum class StorageResult : std::uint8_t {
    OK, MISS, UNAVAILABLE, BUSY, FULL, INVALID_ARGUMENT, INVALID_STATE,
    TOO_LARGE, PARTIAL_WRITE, NO_PROGRESS, CORRUPT, IO_ERROR
};
inline bool storage_result_is_transient(StorageResult r) {
    return r == StorageResult::BUSY || r == StorageResult::UNAVAILABLE;
}
inline bool storage_result_is_unavailable(StorageResult r) { return r == StorageResult::UNAVAILABLE; }
inline bool storage_result_is_write_failure(StorageResult r) {
    return r == StorageResult::FULL || r == StorageResult::PARTIAL_WRITE ||
           r == StorageResult::NO_PROGRESS || r == StorageResult::IO_ERROR;
}
/** SD-shaped filesystem seam. Domain code has no Arduino/FS/SD dependency. */
class NomadNetStorage {
public:
    virtual ~NomadNetStorage() = default;
    virtual bool isAvailable() const = 0;
    virtual StorageResult beginRead(const char* path, std::uint32_t& size) = 0;
    virtual StorageResult readChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count) = 0;
    virtual StorageResult endRead() = 0;
    virtual StorageResult beginWrite(const char* path) = 0;
    virtual StorageResult writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written) = 0;
    virtual StorageResult commitWrite() = 0;
    virtual StorageResult abortWrite() = 0;
    virtual StorageResult remove(const char* path) = 0;
    virtual StorageResult rename(const char* from, const char* to) = 0;
    virtual StorageResult stat(const char* path, std::uint32_t& size) = 0;
    virtual StorageResult beginList(const char* directory) = 0;
    virtual StorageResult nextList(char* path, std::size_t capacity, bool& done) = 0;
    virtual StorageResult endList() = 0;
};
}}}
#endif
