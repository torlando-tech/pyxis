// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "UI/LXMF/MapPackManifest.h"

#include <cstring>
#include <limits>

namespace Pyxis {
namespace {

const std::size_t HEADER_SIZE = 16U;
const std::size_t CRC_SIZE = 4U;
const std::size_t EXTENT_SIZE = 26U;
const std::size_t ROW_SPAN_SIZE = 13U;
const std::uint8_t MAGIC[4] = {'P', 'M', 'P', 'K'};

void putU16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void putU32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint16_t getU16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t getU32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint32_t crc32(const std::uint8_t* input, std::size_t length) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= input[index];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

ManifestResult checkedStringLength(const char* text, std::size_t capacity,
                                   bool pack_id, std::size_t& length) {
    length = 0U;
    while (length < capacity && text[length] != '\0') {
        const unsigned char character = static_cast<unsigned char>(text[length]);
        if (character < 0x20U || character > 0x7eU) return ManifestResult::INVALID_STRING;
        if (pack_id && !((character >= 'a' && character <= 'z') ||
                         (character >= '0' && character <= '9') ||
                         character == '_' || character == '-')) {
            return ManifestResult::INVALID_STRING;
        }
        ++length;
    }
    if (length == 0U || length == capacity) return ManifestResult::INVALID_STRING;
    return ManifestResult::OK;
}

ManifestResult validate(const MapPackManifest& manifest) {
    std::size_t ignored = 0U;
    if (checkedStringLength(manifest.pack_id, sizeof(manifest.pack_id), true, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.name, sizeof(manifest.name), false, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.attribution, sizeof(manifest.attribution), false, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.source, sizeof(manifest.source), false, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.license, sizeof(manifest.license), false, ignored) != ManifestResult::OK) {
        return ManifestResult::INVALID_STRING;
    }
    if (manifest.min_zoom > manifest.max_zoom || manifest.max_zoom > MapPackManifest::MAX_ZOOM) {
        return ManifestResult::INVALID_ZOOM;
    }
    const std::uint8_t expected_count = static_cast<std::uint8_t>(manifest.max_zoom - manifest.min_zoom + 1U);
    if (manifest.extent_count != expected_count || manifest.extent_count > MapPackManifest::MAX_ZOOM_LEVELS) {
        return ManifestResult::INVALID_EXTENT;
    }

    std::uint64_t total = 0U;
    for (std::size_t index = 0U; index < manifest.extent_count; ++index) {
        const ZoomExtent& extent = manifest.extents[index];
        const std::uint8_t expected_zoom = static_cast<std::uint8_t>(manifest.min_zoom + index);
        if (extent.zoom != expected_zoom || extent.interval_count < 1U || extent.interval_count > 2U) {
            return ManifestResult::INVALID_EXTENT;
        }
        const std::uint32_t world_size = UINT32_C(1) << extent.zoom;
        const std::uint32_t world_maximum = world_size - 1U;
        if (extent.y_minimum > extent.y_maximum || extent.y_maximum > world_maximum) {
            return ManifestResult::INVALID_EXTENT;
        }
        std::uint64_t columns = 0U;
        for (std::size_t interval_index = 0U; interval_index < extent.interval_count; ++interval_index) {
            const XInterval& interval = extent.x[interval_index];
            if (interval.minimum > interval.maximum || interval.maximum > world_maximum) {
                return ManifestResult::INVALID_EXTENT;
            }
            columns += static_cast<std::uint64_t>(interval.maximum) - interval.minimum + 1U;
        }
        if (extent.interval_count == 1U) {
            if (extent.x[1].minimum != 0U || extent.x[1].maximum != 0U) {
                return ManifestResult::INVALID_EXTENT;
            }
        } else {
            if (extent.x[0].minimum != 0U || extent.x[1].maximum != world_maximum ||
                extent.x[0].maximum >= extent.x[1].minimum ||
                extent.x[0].maximum + 1U == extent.x[1].minimum) {
                return ManifestResult::INVALID_EXTENT;
            }
        }
        const std::uint64_t rows = static_cast<std::uint64_t>(extent.y_maximum) - extent.y_minimum + 1U;
        const std::uint64_t level_tiles = columns * rows;
        if (level_tiles > std::numeric_limits<std::uint32_t>::max() ||
            total > std::numeric_limits<std::uint32_t>::max() - level_tiles) {
            return ManifestResult::INVALID_TILE_COUNT;
        }
        total += level_tiles;
    }
    if (manifest.tile_count == 0U || total != manifest.tile_count) {
        return ManifestResult::INVALID_TILE_COUNT;
    }
    return ManifestResult::OK;
}

RowSpan readRowSpan(const std::uint8_t* input) {
    RowSpan span = {};
    span.zoom = input[0];
    span.y = getU32(input + 1U);
    span.x_minimum = getU32(input + 5U);
    span.x_maximum = getU32(input + 9U);
    return span;
}

void writeRowSpan(std::uint8_t* output, const RowSpan& span) {
    output[0] = span.zoom;
    putU32(output + 1U, span.y);
    putU32(output + 5U, span.x_minimum);
    putU32(output + 9U, span.x_maximum);
}

ManifestResult validateSparse(const MapPackManifest& manifest,
                              const RowSpan* spans, std::size_t span_count) {
    std::size_t ignored = 0U;
    if (checkedStringLength(manifest.pack_id, sizeof(manifest.pack_id), true, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.name, sizeof(manifest.name), false, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.attribution, sizeof(manifest.attribution), false, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.source, sizeof(manifest.source), false, ignored) != ManifestResult::OK ||
        checkedStringLength(manifest.license, sizeof(manifest.license), false, ignored) != ManifestResult::OK) {
        return ManifestResult::INVALID_STRING;
    }
    if (manifest.min_zoom > manifest.max_zoom || manifest.max_zoom > MapPackManifest::MAX_ZOOM) {
        return ManifestResult::INVALID_ZOOM;
    }
    if (spans == 0 || span_count == 0U || span_count > MapPackManifest::MAX_ROW_SPANS) {
        return ManifestResult::INVALID_EXTENT;
    }
    std::uint32_t zoom_mask = 0U;
    std::uint64_t total = 0U;
    RowSpan previous = {};
    for (std::size_t index = 0U; index < span_count; ++index) {
        const RowSpan& span = spans[index];
        if (span.zoom < manifest.min_zoom || span.zoom > manifest.max_zoom) {
            return ManifestResult::INVALID_EXTENT;
        }
        const std::uint32_t world_maximum = (UINT32_C(1) << span.zoom) - 1U;
        if (span.y > world_maximum || span.x_minimum > span.x_maximum ||
            span.x_maximum > world_maximum) {
            return ManifestResult::INVALID_EXTENT;
        }
        if (index != 0U &&
            (span.zoom < previous.zoom ||
             (span.zoom == previous.zoom && span.y < previous.y) ||
             (span.zoom == previous.zoom && span.y == previous.y &&
              span.x_minimum <= previous.x_maximum + 1U))) {
            return ManifestResult::INVALID_EXTENT;
        }
        zoom_mask |= UINT32_C(1) << span.zoom;
        const std::uint64_t count = static_cast<std::uint64_t>(span.x_maximum) -
                                    span.x_minimum + 1U;
        if (total > std::numeric_limits<std::uint32_t>::max() - count) {
            return ManifestResult::INVALID_TILE_COUNT;
        }
        total += count;
        previous = span;
    }
    std::uint32_t expected_mask = 0U;
    for (std::uint8_t zoom = manifest.min_zoom; zoom <= manifest.max_zoom; ++zoom) {
        expected_mask |= UINT32_C(1) << zoom;
    }
    if (zoom_mask != expected_mask) return ManifestResult::INVALID_ZOOM;
    if (manifest.tile_count == 0U || total != manifest.tile_count) {
        return ManifestResult::INVALID_TILE_COUNT;
    }
    return ManifestResult::OK;
}

void writeString(std::uint8_t* output, std::size_t& position,
                 const char* text, std::size_t length) {
    output[position++] = static_cast<std::uint8_t>(length);
    std::memcpy(output + position, text, length);
    position += length;
}

ManifestResult readString(const std::uint8_t* input, std::size_t payload_end,
                          std::size_t& position, char* output,
                          std::size_t capacity, bool pack_id) {
    if (position >= payload_end) return ManifestResult::BAD_LENGTH;
    const std::size_t length = input[position++];
    if (length == 0U || length >= capacity || length > payload_end - position) {
        return length >= capacity ? ManifestResult::INVALID_STRING : ManifestResult::BAD_LENGTH;
    }
    for (std::size_t index = 0U; index < length; ++index) {
        const unsigned char character = input[position + index];
        if (character < 0x20U || character > 0x7eU ||
            (pack_id && !((character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') ||
                          character == '_' || character == '-'))) {
            return ManifestResult::INVALID_STRING;
        }
    }
    std::memcpy(output, input + position, length);
    output[length] = '\0';
    position += length;
    return ManifestResult::OK;
}

}  // namespace

ManifestResult MapPackManifest::serialize(const MapPackManifest& manifest,
                                          std::uint8_t* output,
                                          std::size_t capacity,
                                          std::size_t& written) {
    const ManifestResult validity = validate(manifest);
    if (validity != ManifestResult::OK) return validity;

    std::size_t lengths[5] = {0U, 0U, 0U, 0U, 0U};
    (void)checkedStringLength(manifest.pack_id, sizeof(manifest.pack_id), true, lengths[0]);
    (void)checkedStringLength(manifest.name, sizeof(manifest.name), false, lengths[1]);
    (void)checkedStringLength(manifest.attribution, sizeof(manifest.attribution), false, lengths[2]);
    (void)checkedStringLength(manifest.source, sizeof(manifest.source), false, lengths[3]);
    (void)checkedStringLength(manifest.license, sizeof(manifest.license), false, lengths[4]);
    std::size_t required = HEADER_SIZE + CRC_SIZE + 7U + EXTENT_SIZE * manifest.extent_count;
    for (std::size_t index = 0U; index < 5U; ++index) required += 1U + lengths[index];
    if (output == 0 || capacity < required) return ManifestResult::INSUFFICIENT_CAPACITY;

    std::uint8_t temporary[MAX_SERIALIZED_SIZE] = {};
    std::memcpy(temporary, MAGIC, sizeof(MAGIC));
    temporary[4] = LEGACY_FORMAT_VERSION;
    temporary[5] = 0U;
    putU16(temporary + 6U, static_cast<std::uint16_t>(HEADER_SIZE));
    putU32(temporary + 8U, static_cast<std::uint32_t>(required));
    putU32(temporary + 12U, 0U);
    std::size_t position = HEADER_SIZE;
    writeString(temporary, position, manifest.pack_id, lengths[0]);
    writeString(temporary, position, manifest.name, lengths[1]);
    writeString(temporary, position, manifest.attribution, lengths[2]);
    writeString(temporary, position, manifest.source, lengths[3]);
    writeString(temporary, position, manifest.license, lengths[4]);
    temporary[position++] = manifest.min_zoom;
    temporary[position++] = manifest.max_zoom;
    temporary[position++] = manifest.extent_count;
    putU32(temporary + position, manifest.tile_count); position += 4U;
    for (std::size_t index = 0U; index < manifest.extent_count; ++index) {
        const ZoomExtent& extent = manifest.extents[index];
        temporary[position++] = extent.zoom;
        temporary[position++] = extent.interval_count;
        putU32(temporary + position, extent.y_minimum); position += 4U;
        putU32(temporary + position, extent.y_maximum); position += 4U;
        for (std::size_t interval = 0U; interval < 2U; ++interval) {
            putU32(temporary + position, extent.x[interval].minimum); position += 4U;
            putU32(temporary + position, extent.x[interval].maximum); position += 4U;
        }
    }
    putU32(temporary + position, crc32(temporary, position)); position += CRC_SIZE;
    if (position != required) return ManifestResult::BAD_LENGTH;
    std::memcpy(output, temporary, required);
    written = required;
    return ManifestResult::OK;
}

ManifestResult MapPackManifest::serializeSparse(const MapPackManifest& manifest,
                                                const RowSpan* spans,
                                                std::size_t span_count,
                                                std::uint8_t* output,
                                                std::size_t capacity,
                                                std::size_t& written) {
    const ManifestResult validity = validateSparse(manifest, spans, span_count);
    if (validity != ManifestResult::OK) return validity;
    std::size_t lengths[5] = {0U, 0U, 0U, 0U, 0U};
    (void)checkedStringLength(manifest.pack_id, sizeof(manifest.pack_id), true, lengths[0]);
    (void)checkedStringLength(manifest.name, sizeof(manifest.name), false, lengths[1]);
    (void)checkedStringLength(manifest.attribution, sizeof(manifest.attribution), false, lengths[2]);
    (void)checkedStringLength(manifest.source, sizeof(manifest.source), false, lengths[3]);
    (void)checkedStringLength(manifest.license, sizeof(manifest.license), false, lengths[4]);
    std::size_t required = HEADER_SIZE + CRC_SIZE + 8U + ROW_SPAN_SIZE * span_count;
    for (std::size_t index = 0U; index < 5U; ++index) required += 1U + lengths[index];
    if (output == 0 || capacity < required) return ManifestResult::INSUFFICIENT_CAPACITY;

    std::uint8_t temporary[MAX_SERIALIZED_SIZE] = {};
    std::memcpy(temporary, MAGIC, sizeof(MAGIC));
    temporary[4] = FORMAT_VERSION;
    temporary[5] = 0U;
    putU16(temporary + 6U, static_cast<std::uint16_t>(HEADER_SIZE));
    putU32(temporary + 8U, static_cast<std::uint32_t>(required));
    putU32(temporary + 12U, 0U);
    std::size_t position = HEADER_SIZE;
    writeString(temporary, position, manifest.pack_id, lengths[0]);
    writeString(temporary, position, manifest.name, lengths[1]);
    writeString(temporary, position, manifest.attribution, lengths[2]);
    writeString(temporary, position, manifest.source, lengths[3]);
    writeString(temporary, position, manifest.license, lengths[4]);
    temporary[position++] = manifest.min_zoom;
    temporary[position++] = manifest.max_zoom;
    putU16(temporary + position, static_cast<std::uint16_t>(span_count)); position += 2U;
    putU32(temporary + position, manifest.tile_count); position += 4U;
    for (std::size_t index = 0U; index < span_count; ++index) {
        writeRowSpan(temporary + position, spans[index]);
        position += ROW_SPAN_SIZE;
    }
    putU32(temporary + position, crc32(temporary, position)); position += CRC_SIZE;
    if (position != required) return ManifestResult::BAD_LENGTH;
    std::memcpy(output, temporary, required);
    written = required;
    return ManifestResult::OK;
}

ManifestResult MapPackManifest::parse(const std::uint8_t* input,
                                      std::size_t length,
                                      MapPackManifest& output) {
    if (input == 0 || length < HEADER_SIZE + CRC_SIZE) return ManifestResult::BAD_LENGTH;
    if (std::memcmp(input, MAGIC, sizeof(MAGIC)) != 0) return ManifestResult::BAD_MAGIC;
    const std::uint8_t version = input[4];
    if (version != LEGACY_FORMAT_VERSION && version != FORMAT_VERSION) {
        return ManifestResult::UNSUPPORTED_VERSION;
    }
    if (input[5] != 0U || getU16(input + 6U) != HEADER_SIZE || getU32(input + 12U) != 0U) {
        return ManifestResult::BAD_HEADER;
    }
    if (getU32(input + 8U) != length || length > MAX_SERIALIZED_SIZE) return ManifestResult::BAD_LENGTH;
    if (getU32(input + length - CRC_SIZE) != crc32(input, length - CRC_SIZE)) return ManifestResult::BAD_CRC;

    const std::size_t payload_end = length - CRC_SIZE;
    std::size_t position = HEADER_SIZE;
    MapPackManifest candidate = {};
    ManifestResult result = readString(input, payload_end, position, candidate.pack_id, sizeof(candidate.pack_id), true);
    if (result != ManifestResult::OK) return result;
    result = readString(input, payload_end, position, candidate.name, sizeof(candidate.name), false);
    if (result != ManifestResult::OK) return result;
    result = readString(input, payload_end, position, candidate.attribution, sizeof(candidate.attribution), false);
    if (result != ManifestResult::OK) return result;
    result = readString(input, payload_end, position, candidate.source, sizeof(candidate.source), false);
    if (result != ManifestResult::OK) return result;
    result = readString(input, payload_end, position, candidate.license, sizeof(candidate.license), false);
    if (result != ManifestResult::OK) return result;
    candidate.format_version = version;
    if (version == LEGACY_FORMAT_VERSION) {
        if (payload_end - position < 7U) return ManifestResult::BAD_LENGTH;
        candidate.min_zoom = input[position++];
        candidate.max_zoom = input[position++];
        candidate.extent_count = input[position++];
        candidate.tile_count = getU32(input + position); position += 4U;
        if (candidate.extent_count > MAX_ZOOM_LEVELS ||
            static_cast<std::size_t>(candidate.extent_count) > (payload_end - position) / EXTENT_SIZE) {
            return ManifestResult::INVALID_EXTENT;
        }
        for (std::size_t index = 0U; index < candidate.extent_count; ++index) {
            ZoomExtent& extent = candidate.extents[index];
            extent.zoom = input[position++];
            extent.interval_count = input[position++];
            extent.y_minimum = getU32(input + position); position += 4U;
            extent.y_maximum = getU32(input + position); position += 4U;
            for (std::size_t interval = 0U; interval < 2U; ++interval) {
                extent.x[interval].minimum = getU32(input + position); position += 4U;
                extent.x[interval].maximum = getU32(input + position); position += 4U;
            }
        }
        if (position != payload_end) return ManifestResult::BAD_LENGTH;
        result = validate(candidate);
    } else {
        if (payload_end - position < 8U) return ManifestResult::BAD_LENGTH;
        candidate.min_zoom = input[position++];
        candidate.max_zoom = input[position++];
        candidate.row_span_count = getU16(input + position); position += 2U;
        candidate.tile_count = getU32(input + position); position += 4U;
        if (candidate.row_span_count == 0U || candidate.row_span_count > MAX_ROW_SPANS ||
            static_cast<std::size_t>(candidate.row_span_count) >
                (payload_end - position) / ROW_SPAN_SIZE) {
            return ManifestResult::INVALID_EXTENT;
        }
        candidate.row_span_bytes = input + position;
        if (candidate.min_zoom > candidate.max_zoom || candidate.max_zoom > MAX_ZOOM) {
            return ManifestResult::INVALID_ZOOM;
        }
        std::uint32_t zoom_mask = 0U;
        std::uint64_t total = 0U;
        RowSpan previous = {};
        for (std::size_t index = 0U; index < candidate.row_span_count; ++index) {
            const RowSpan span = readRowSpan(input + position);
            const std::uint32_t world_maximum = span.zoom <= MAX_ZOOM
                ? (UINT32_C(1) << span.zoom) - 1U : 0U;
            if (span.zoom < candidate.min_zoom || span.zoom > candidate.max_zoom ||
                span.y > world_maximum || span.x_minimum > span.x_maximum ||
                span.x_maximum > world_maximum ||
                (index != 0U &&
                 (span.zoom < previous.zoom ||
                  (span.zoom == previous.zoom && span.y < previous.y) ||
                  (span.zoom == previous.zoom && span.y == previous.y &&
                   span.x_minimum <= previous.x_maximum + 1U)))) {
                return ManifestResult::INVALID_EXTENT;
            }
            const std::uint64_t count = static_cast<std::uint64_t>(span.x_maximum) -
                                        span.x_minimum + 1U;
            if (total > std::numeric_limits<std::uint32_t>::max() - count) {
                return ManifestResult::INVALID_TILE_COUNT;
            }
            total += count;
            zoom_mask |= UINT32_C(1) << span.zoom;
            previous = span;
            position += ROW_SPAN_SIZE;
        }
        if (position != payload_end) return ManifestResult::BAD_LENGTH;
        std::uint32_t expected_mask = 0U;
        for (std::uint8_t zoom = candidate.min_zoom; zoom <= candidate.max_zoom; ++zoom) {
            expected_mask |= UINT32_C(1) << zoom;
        }
        result = zoom_mask != expected_mask ? ManifestResult::INVALID_ZOOM :
            (candidate.tile_count == 0U || total != candidate.tile_count
                ? ManifestResult::INVALID_TILE_COUNT : ManifestResult::OK);
    }
    if (result != ManifestResult::OK) return result;
    output = candidate;
    return ManifestResult::OK;
}

bool MapPackManifest::covers(const Hardware::TDeck::TileKey& key) const {
    if (key.zoom > MAX_ZOOM) return false;
    const std::uint32_t world_size = UINT32_C(1) << key.zoom;
    if (key.x >= world_size || key.y >= world_size ||
        key.zoom < min_zoom || key.zoom > max_zoom) return false;
    if (format_version == FORMAT_VERSION) {
        if (row_span_bytes == 0 || row_span_count == 0U || row_span_count > MAX_ROW_SPANS) return false;
        for (std::size_t index = 0U; index < row_span_count; ++index) {
            const RowSpan span = readRowSpan(row_span_bytes + index * ROW_SPAN_SIZE);
            if (span.zoom > key.zoom || (span.zoom == key.zoom && span.y > key.y)) return false;
            if (span.zoom == key.zoom && span.y == key.y &&
                key.x >= span.x_minimum && key.x <= span.x_maximum) return true;
        }
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(key.zoom - min_zoom);
    if (index >= extent_count) return false;
    const ZoomExtent& extent = extents[index];
    if (extent.zoom != key.zoom || key.y < extent.y_minimum || key.y > extent.y_maximum ||
        extent.interval_count < 1U || extent.interval_count > 2U) return false;
    for (std::size_t interval = 0U; interval < extent.interval_count; ++interval) {
        if (key.x >= extent.x[interval].minimum && key.x <= extent.x[interval].maximum) return true;
    }
    return false;
}

}  // namespace Pyxis
