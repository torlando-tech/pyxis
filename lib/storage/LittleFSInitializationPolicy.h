#pragma once

#include <cstddef>
#include <cstdint>

namespace Storage {

constexpr std::size_t LITTLEFS_BLANK_SCAN_CHUNK_SIZE = 4096;

template <typename Mount, typename ResolvePartition, typename ReadPartition,
          typename FormatAndMount>
bool mount_or_initialize_erased_littlefs(
    Mount mount,
    ResolvePartition resolve_partition,
    ReadPartition read_partition,
    FormatAndMount format_and_mount) {
    if (mount()) return true;

    std::size_t partition_size = 0;
    if (!resolve_partition(partition_size) || partition_size == 0) return false;

    uint8_t buffer[LITTLEFS_BLANK_SCAN_CHUNK_SIZE];
    std::size_t offset = 0;
    while (offset < partition_size) {
        const std::size_t remaining = partition_size - offset;
        const std::size_t chunk =
            remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (!read_partition(offset, buffer, chunk)) return false;
        for (std::size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != 0xFF) return false;
        }
        offset += chunk;
    }

    return format_and_mount();
}

}  // namespace Storage
