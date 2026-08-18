#include "NomadNetPartialScheduler.h"

#include <algorithm>

namespace UI::LXMF::NomadNet {

bool PartialScheduler::due(uint32_t now_ms, uint32_t due_at_ms) noexcept {
    return static_cast<int32_t>(now_ms - due_at_ms) >= 0;
}

uint32_t PartialScheduler::next_token() noexcept {
    ++_token_sequence;
    if (_token_sequence == 0) ++_token_sequence;
    return _token_sequence;
}

void PartialScheduler::configure(const CompactPage& page,
                                 uint32_t page_generation,
                                 uint32_t now_ms) noexcept {
    _entries = {};
    _count = page_generation == 0 ? 0 :
        std::min(page.partials().size(), _entries.size());
    _page_generation = page_generation;
    _next_scan_index = 0;
    _in_flight_token = 0;
    _in_flight_index = 0;
    for (std::size_t i = 0; i < _count; ++i) {
        _entries[i].refresh_interval_ms = page.partials()[i].refresh_interval_ms;
        _entries[i].descriptor_hash = page.partials()[i].descriptor_hash;
        _entries[i].due_at_ms = now_ms;
        _entries[i].pending = true;
    }
}

void PartialScheduler::configure(const Document& document,
                                 uint32_t page_generation,
                                 uint32_t now_ms) noexcept {
    _entries = {};
    _count = page_generation == 0 ? 0 :
        std::min(document.partials.size(), _entries.size());
    _page_generation = page_generation;
    _next_scan_index = 0;
    _in_flight_token = 0;
    _in_flight_index = 0;
    for (std::size_t i = 0; i < _count; ++i) {
        _entries[i].refresh_interval_ms = document.partials[i].refresh_interval_ms;
        _entries[i].descriptor_hash = document.partials[i].descriptor_hash;
        _entries[i].due_at_ms = now_ms;
        _entries[i].pending = true;
    }
}

void PartialScheduler::cancel(uint32_t page_generation) noexcept {
    if (_page_generation != page_generation) return;
    _entries = {};
    _count = 0;
    _page_generation = 0;
    _next_scan_index = 0;
    _in_flight_token = 0;
    _in_flight_index = 0;
}

bool PartialScheduler::poll(uint32_t now_ms, bool browser_active,
                            bool owner_available,
                            PartialRequest& request) noexcept {
    if (!browser_active || !owner_available || _page_generation == 0 ||
        _in_flight_token != 0) return false;
    for (std::size_t offset = 0; offset < _count; ++offset) {
        const std::size_t i = (_next_scan_index + offset) % _count;
        Entry& entry = _entries[i];
        if (!entry.pending || entry.in_flight || !due(now_ms, entry.due_at_ms)) continue;
        entry.pending = false;
        entry.in_flight = true;
        entry.started_at_ms = now_ms;
        entry.request_token = next_token();
        ++entry.partial_generation;
        if (entry.partial_generation == 0) ++entry.partial_generation;
        _in_flight_token = entry.request_token;
        _in_flight_index = static_cast<uint16_t>(i);
        request.partial_index = static_cast<uint16_t>(i);
        request.page_generation = _page_generation;
        request.partial_generation = entry.partial_generation;
        request.request_token = entry.request_token;
        request.descriptor_hash = entry.descriptor_hash;
        _next_scan_index = static_cast<uint8_t>((i + 1U) % _count);
        return true;
    }
    return false;
}

bool PartialScheduler::complete(const PartialRequest& request, bool success,
                                uint32_t now_ms) noexcept {
    if (_page_generation == 0 || request.page_generation != _page_generation ||
        request.partial_index >= _count || request.request_token == 0 ||
        request.request_token != _in_flight_token ||
        request.partial_index != _in_flight_index) return false;
    Entry& entry = _entries[request.partial_index];
    if (!entry.in_flight || entry.request_token != request.request_token ||
        entry.partial_generation != request.partial_generation ||
        entry.descriptor_hash != request.descriptor_hash) return false;

    entry.in_flight = false;
    entry.request_token = 0;
    _in_flight_token = 0;
    _in_flight_index = 0;
    if (success) {
        if (entry.refresh_interval_ms != 0) {
            entry.due_at_ms = entry.started_at_ms + entry.refresh_interval_ms + 1U;
            entry.pending = true;
        }
        return true;
    }

    if (entry.refresh_interval_ms == 0) {
        entry.pending = false;
        return true;
    }
    entry.due_at_ms = now_ms + entry.refresh_interval_ms + 1U;
    entry.pending = true;
    return true;
}

bool PartialScheduler::request_now(std::size_t partial_index,
                                   uint32_t page_generation,
                                   uint32_t now_ms) noexcept {
    if (page_generation != _page_generation || partial_index >= _count ||
        _entries[partial_index].in_flight) return false;
    Entry& entry = _entries[partial_index];
    entry.due_at_ms = now_ms;
    entry.pending = true;
    return true;
}

} // namespace UI::LXMF::NomadNet
