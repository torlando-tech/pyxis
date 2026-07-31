// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_MAP_TILE_LOOKUP_POLICY_H
#define UI_LXMF_MAP_TILE_LOOKUP_POLICY_H

#include <cstddef>
#include <cstdint>

#include "MapScreenPresenter.h"

namespace Pyxis {

/** Portable policy for pack -> live cache -> optional network orchestration. */
class MapTileLookupPolicy {
public:
    enum class LocalSource : std::uint8_t { PACK = 0, LIVE_STORE };
    typedef MapTileLoadResult (*ReadLocalSource)(void*, LocalSource);

    static MapTileLoadResult readLocal(void* context, ReadLocalSource read) {
        if (read == NULL) return MapTileLoadResult::IO_ERROR;
        const MapTileLoadResult pack = read(context, LocalSource::PACK);
        if (pack == MapTileLoadResult::READY) return pack;
        const MapTileLoadResult live = read(context, LocalSource::LIVE_STORE);
        return resolveLocal(pack, live);
    }

    static MapTileLoadResult resolveLocal(MapTileLoadResult pack,
                                          MapTileLoadResult live) {
        if (pack == MapTileLoadResult::READY) return pack;
        if (live != MapTileLoadResult::MISS) return live;
        if (pack == MapTileLoadResult::IO_ERROR ||
            pack == MapTileLoadResult::STORAGE_UNAVAILABLE) return pack;
        return MapTileLoadResult::MISS;
    }

    static bool shouldStartOnline(MapTileLoadResult local,
                                  bool enabled,
                                  bool visible,
                                  bool decode_failed,
                                  std::uint32_t request_epoch,
                                  std::uint32_t current_epoch) {
        return enabled && visible && !decode_failed &&
            request_epoch == current_epoch &&
            (local == MapTileLoadResult::MISS ||
             local == MapTileLoadResult::INVALID_PNG);
    }
};

}  // namespace Pyxis

#endif
