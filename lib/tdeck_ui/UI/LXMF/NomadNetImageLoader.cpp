// NomadNet page-image loader implementation. See NomadNetImageLoader.h.
#include "NomadNetImageLoader.h"

#include <new>
#include <utility>

namespace UI::LXMF::NomadNet {

void ImageLoader::reset_entries() {
    for (std::size_t i = 0; i < CAPACITY; ++i) {
        _entries[i].data = ImageLoadEntry{};
        _entries[i].state = ImageState::SKIPPED;
        _entries[i].terminal_failure = false;
    }
}

ImageLoader::Entry* ImageLoader::find(std::size_t index) const {
    return (index < _count) ? const_cast<Entry*>(&_entries[index]) : nullptr;
}

bool ImageLoader::configure(std::size_t image_count, const ImageLoadEntry* entries,
                            const std::string& resolved_destination,
                            std::uint32_t page_generation, ImagePolicy policy,
                            bool loopback, std::uint32_t rtt_ms, std::uint32_t edr_bps) {
    try {
        _generation = page_generation;
        _resolved_destination = resolved_destination;
        if (image_count > CAPACITY) image_count = CAPACITY;
        // Reference policy gate: ALWAYS loads; NEVER never does; MANUAL loads
        // nothing until the user explicitly requests a reload (entries start
        // SKIPPED); AUTO uses the RTT/EDR gate (loopback always passes).
        const bool gate = (policy == ImagePolicy::ALWAYS) ||
                          (policy == ImagePolicy::AUTO &&
                           auto_gate_allows(loopback, rtt_ms, edr_bps));
        for (std::size_t i = 0; i < CAPACITY; ++i) {
            _entries[i].data = ImageLoadEntry{};
            _entries[i].state = ImageState::SKIPPED;
            _entries[i].terminal_failure = false;
        }
        _count = 0;
        for (std::size_t i = 0; i < image_count; ++i) {
            const ImageLoadEntry& source = entries[i];
            const std::string destination = source.same_destination
                ? resolved_destination : source.destination_hex;
            if (destination.empty()) {
                // Unresolvable destination (malformed url): skip, never fetch.
                continue;
            }
            ImageLoadEntry entry;
            entry.image_index = source.image_index;
            entry.same_destination = source.same_destination;
            entry.destination_hex = destination;
            entry.path = source.path;
            entry.cache_key = image_cache_key(destination, source.path);
            if (!gate) {
                _entries[_count] = Entry{};
                _entries[_count].data = std::move(entry);
                _entries[_count].state = ImageState::SKIPPED;
            } else {
                _entries[_count] = Entry{};
                _entries[_count].data = std::move(entry);
                _entries[_count].state = ImageState::PENDING;
            }
            ++_count;
        }
        _active = _count;
        return true;
    } catch (const std::bad_alloc&) {
        reset_entries();
        _count = 0;
        _active = 0;
        return false;
    }
}

ImageAction ImageLoader::poll() {
    // Strictly sequential: at most one entry may be REQUESTING, and it is
    // always the first non-terminal entry in document order. The position is
    // derived from the states, so no cursor can drift.
    for (std::size_t i = 0; i < _count; ++i) {
        if (_entries[i].state == ImageState::REQUESTING) return ImageAction::NONE;
        if (_entries[i].state == ImageState::PENDING) {
            _entries[i].state = ImageState::REQUESTING;
            _active = i;
            return ImageAction::SEND_REQUEST;
        }
    }
    return ImageAction::NONE;
}

std::size_t ImageLoader::request_images() {
    std::size_t re_admitted = 0;
    for (std::size_t i = 0; i < _count; ++i) {
        if (_entries[i].state == ImageState::SKIPPED && !_entries[i].terminal_failure) {
            _entries[i].state = ImageState::PENDING;
            ++re_admitted;
        }
    }
    return re_admitted;
}

bool ImageLoader::finish_active(bool success) {
    if (_active >= _count) return false;
    if (_entries[_active].state != ImageState::REQUESTING) return false;
    _entries[_active].state = success ? ImageState::LOADED : ImageState::FAILED;
    if (!success) _entries[_active].terminal_failure = true;
    _active = _count; // no in-flight request
    return true;
}

void ImageLoader::fail_image(uint16_t image_index, bool terminal) {
    for (std::size_t i = 0; i < _count; ++i) {
        if (_entries[i].data.image_index == image_index) {
            _entries[i].state = ImageState::FAILED;
            if (terminal) _entries[i].terminal_failure = true;
            if (_active == i) _active = _count; // the in-flight one died
            return;
        }
    }
}

void ImageLoader::cancel_all() {
    reset_entries();
    _count = 0;
    _active = 0;
}

ImageState ImageLoader::state_of(uint16_t image_index) const {
    for (std::size_t i = 0; i < _count; ++i) {
        if (_entries[i].data.image_index == image_index) return _entries[i].state;
    }
    return ImageState::SKIPPED;
}

const ImageLoadEntry* ImageLoader::active_entry() const {
    if (_active >= _count) return nullptr;
    if (_entries[_active].state != ImageState::REQUESTING) return nullptr;
    return &_entries[_active].data;
}

std::size_t ImageLoader::loaded_count() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < _count; ++i)
        if (_entries[i].state == ImageState::LOADED) ++n;
    return n;
}

std::size_t ImageLoader::failed_count() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < _count; ++i)
        if (_entries[i].state == ImageState::FAILED) ++n;
    return n;
}

bool ImageLoader::all_done() const {
    for (std::size_t i = 0; i < _count; ++i) {
        if (_entries[i].state == ImageState::PENDING ||
            _entries[i].state == ImageState::REQUESTING)
            return false;
    }
    return true;
}

std::string ImageLoader::active_destination() const {
    const auto* entry = active_entry();
    return entry ? entry->destination_hex : std::string();
}

} // namespace UI::LXMF::NomadNet
