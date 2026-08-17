#pragma once

#include "NomadNetCompactPage.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace UI::LXMF::NomadNet {

struct PartialRequest {
    uint16_t partial_index = 0;
    uint32_t page_generation = 0;
    uint32_t partial_generation = 0;
    uint32_t request_token = 0;
    std::array<uint8_t, 32> descriptor_hash{};
};

// Allocation-free, single-owner scheduler for bounded dynamic partial work.
// It never retains CompactPage pointers or response bytes. The caller owns
// transport and must validate completion through the returned generation/token.
class PartialScheduler {
public:

    void configure(const CompactPage& page, uint32_t page_generation,
                   uint32_t now_ms) noexcept;
    void configure(const Document& document, uint32_t page_generation,
                   uint32_t now_ms) noexcept;
    void cancel(uint32_t page_generation) noexcept;

    bool poll(uint32_t now_ms, bool browser_active, bool owner_available,
              PartialRequest& request) noexcept;
    bool complete(const PartialRequest& request, bool success,
                  uint32_t now_ms) noexcept;
    bool defer(const PartialRequest& request) noexcept;
    bool request_now(std::size_t partial_index, uint32_t page_generation,
                     uint32_t now_ms) noexcept;

    bool empty() const noexcept { return _page_generation == 0 || _count == 0; }
    bool in_flight() const noexcept { return _in_flight_token != 0; }
    std::size_t size() const noexcept { return _count; }
    uint32_t page_generation() const noexcept { return _page_generation; }

private:
    struct Entry {
        uint32_t refresh_interval_ms = 0;
        uint32_t due_at_ms = 0;
        uint32_t started_at_ms = 0;
        uint32_t request_token = 0;
        uint32_t partial_generation = 0;
        std::array<uint8_t, 32> descriptor_hash{};
        bool pending = false;
        bool in_flight = false;
    };

    static bool due(uint32_t now_ms, uint32_t due_at_ms) noexcept;
    uint32_t next_token() noexcept;

    std::array<Entry, CompactPage::MAX_PARTIALS> _entries{};
    std::size_t _count = 0;
    uint32_t _page_generation = 0;
    uint32_t _token_sequence = 0;
    uint8_t _next_scan_index = 0;
    uint32_t _in_flight_token = 0;
    uint16_t _in_flight_index = 0;
};

} // namespace UI::LXMF::NomadNet
