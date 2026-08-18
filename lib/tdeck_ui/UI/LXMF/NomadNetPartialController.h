#pragma once

#include "NomadNetForm.h"
#include "NomadNetPartialScheduler.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace UI::LXMF::NomadNet {

class PartialController {
public:
    static constexpr std::size_t MAX_RESPONSE_BYTES = 16 * 1024;
    static constexpr std::size_t MAX_RESPONSE_WIRE_BYTES = MAX_RESPONSE_BYTES + 64;
    static constexpr std::size_t MAX_EXPANDED_SOURCE_BYTES =
        DocumentParser::MAX_DOCUMENT_BYTES;

    PartialController() = default;
    ~PartialController() { clear_lease(); }
    PartialController(const PartialController&) = delete;
    PartialController& operator=(const PartialController&) = delete;

    bool prepare(const PartialRequest& request, const CompactPage& page,
                 const uint8_t* request_data, std::size_t request_size) noexcept;
    void reset_page(std::size_t base_source_bytes) noexcept;
    void abandon_request() noexcept { clear_lease(); }
    void cancel() noexcept;

    bool active() const noexcept { return _active; }
    const PartialRequest& request() const noexcept { return _request; }
    const char* descriptor_data() const noexcept { return _descriptor.data(); }
    std::size_t descriptor_size() const noexcept { return _descriptor_size; }
    const char* url_data() const noexcept { return _url.data(); }
    std::size_t url_size() const noexcept { return _url_size; }
    const char* selectors_data() const noexcept { return _selectors.data(); }
    std::size_t selectors_size() const noexcept { return _selectors_size; }
    const uint8_t* request_data() const noexcept { return _request_data.data(); }
    std::size_t request_size() const noexcept { return _request_size; }

    bool matches(const PartialRequest& request) const noexcept;
    bool matches(const PartialRequest& request,
                 const CompactPage& page) const noexcept;
    bool can_accept_fragment(std::size_t partial_index,
                             std::size_t source_bytes) const noexcept;
    bool commit_fragment(std::size_t partial_index,
                         std::size_t source_bytes) noexcept;
    std::size_t expanded_source_bytes() const noexcept;

private:
    void clear_lease() noexcept;

    PartialRequest _request{};
    std::array<char, DocumentParser::MAX_PARTIAL_DESCRIPTOR_BYTES + 1> _descriptor{};
    std::array<char, DocumentParser::MAX_PARTIAL_URL_BYTES + 1> _url{};
    std::array<char, FormState::MAX_SELECTOR_BYTES + 1> _selectors{};
    std::array<uint8_t, FormState::MAX_ENCODED_BYTES> _request_data{};
    std::array<uint32_t, CompactPage::MAX_PARTIALS> _fragment_source_bytes{};
    std::size_t _base_source_bytes = 0;
    uint16_t _descriptor_size = 0;
    uint16_t _url_size = 0;
    uint16_t _selectors_size = 0;
    uint16_t _request_size = 0;
    bool _active = false;
};

} // namespace UI::LXMF::NomadNet
