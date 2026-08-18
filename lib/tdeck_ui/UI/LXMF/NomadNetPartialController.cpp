#include "NomadNetPartialController.h"

#include "NomadNetPartialHash.h"

#include <algorithm>
#include <cstring>

namespace UI::LXMF::NomadNet {

namespace {

void wipe(void* memory, std::size_t size) noexcept {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = 0;
}

std::array<uint8_t, 32> descriptor_identity_hash(
        CompactPage::TextView descriptor) noexcept {
    std::array<char, DocumentParser::MAX_PARTIAL_DESCRIPTOR_BYTES + 2> identity{};
    std::size_t size = 0;
    for (std::size_t index = 0; index < descriptor.size(); ++index) {
        const char value = descriptor.data()[index];
        identity[size++] = value == '`' ? '|' : value;
    }
    return partial_descriptor_sha256(identity.data(), size);
}

} // namespace

bool PartialController::prepare(const PartialRequest& request,
                                const CompactPage& page,
                                const uint8_t* request_data,
                                std::size_t request_size) noexcept {
    clear_lease();
    if (request.page_generation == 0 || request.request_token == 0 ||
            request.partial_generation == 0 ||
            request.partial_index >= page.partials().size() ||
            request_size > _request_data.size() ||
            (!request_data && request_size != 0))
        return false;
    const auto& partial = page.partials()[request.partial_index];
    if (partial.descriptor_hash != request.descriptor_hash) return false;
    const auto descriptor = page.partial_descriptor(partial);
    const auto url = page.partial_url(partial);
    const auto selectors = page.partial_selectors(partial);
    if ((!descriptor.data() && !descriptor.empty()) ||
            (!url.data() && !url.empty()) ||
            (!selectors.data() && !selectors.empty()) ||
            descriptor.size() > DocumentParser::MAX_PARTIAL_DESCRIPTOR_BYTES ||
            url.size() > DocumentParser::MAX_PARTIAL_URL_BYTES ||
            selectors.size() > FormState::MAX_SELECTOR_BYTES ||
            descriptor_identity_hash(descriptor) != request.descriptor_hash)
        return false;

    if (!descriptor.empty())
        std::memcpy(_descriptor.data(), descriptor.data(), descriptor.size());
    if (!url.empty()) std::memcpy(_url.data(), url.data(), url.size());
    if (!selectors.empty())
        std::memcpy(_selectors.data(), selectors.data(), selectors.size());
    if (request_size != 0)
        std::memcpy(_request_data.data(), request_data, request_size);
    _descriptor_size = static_cast<uint16_t>(descriptor.size());
    _url_size = static_cast<uint16_t>(url.size());
    _selectors_size = static_cast<uint16_t>(selectors.size());
    _request_size = static_cast<uint16_t>(request_size);
    _request = request;
    _active = true;
    return true;
}

void PartialController::reset_page(std::size_t base_source_bytes) noexcept {
    clear_lease();
    _fragment_source_bytes = {};
    _base_source_bytes = std::min(base_source_bytes, MAX_EXPANDED_SOURCE_BYTES);
}

void PartialController::cancel() noexcept {
    clear_lease();
    _fragment_source_bytes = {};
    _base_source_bytes = 0;
}

bool PartialController::matches(const PartialRequest& request) const noexcept {
    return _active && request.partial_index == _request.partial_index &&
        request.page_generation == _request.page_generation &&
        request.partial_generation == _request.partial_generation &&
        request.request_token == _request.request_token &&
        request.descriptor_hash == _request.descriptor_hash;
}

bool PartialController::matches(const PartialRequest& request,
                                const CompactPage& page) const noexcept {
    if (!matches(request) || request.partial_index >= page.partials().size())
        return false;
    const auto& partial = page.partials()[request.partial_index];
    const auto descriptor = page.partial_descriptor(partial);
    return partial.descriptor_hash == request.descriptor_hash &&
        descriptor.size() == _descriptor_size &&
        (_descriptor_size == 0 || std::memcmp(
            descriptor.data(), _descriptor.data(), _descriptor_size) == 0);
}

std::size_t PartialController::expanded_source_bytes() const noexcept {
    std::size_t total = _base_source_bytes;
    for (const uint32_t bytes : _fragment_source_bytes) {
        if (bytes > MAX_EXPANDED_SOURCE_BYTES -
                std::min(total, MAX_EXPANDED_SOURCE_BYTES))
            return MAX_EXPANDED_SOURCE_BYTES + 1;
        total += bytes;
    }
    return total;
}

bool PartialController::can_accept_fragment(std::size_t partial_index,
                                            std::size_t source_bytes) const noexcept {
    if (!_active || partial_index != _request.partial_index ||
            partial_index >= _fragment_source_bytes.size() ||
            source_bytes > MAX_RESPONSE_BYTES)
        return false;
    const std::size_t current = expanded_source_bytes();
    if (current > MAX_EXPANDED_SOURCE_BYTES) return false;
    const std::size_t old = _fragment_source_bytes[partial_index];
    const std::size_t without_old = current >= old ? current - old : 0;
    return source_bytes <= MAX_EXPANDED_SOURCE_BYTES -
        std::min(without_old, MAX_EXPANDED_SOURCE_BYTES);
}

bool PartialController::commit_fragment(std::size_t partial_index,
                                        std::size_t source_bytes) noexcept {
    if (!can_accept_fragment(partial_index, source_bytes)) return false;
    _fragment_source_bytes[partial_index] = static_cast<uint32_t>(source_bytes);
    return true;
}

void PartialController::clear_lease() noexcept {
    wipe(_descriptor.data(), _descriptor.size());
    wipe(_url.data(), _url.size());
    wipe(_selectors.data(), _selectors.size());
    wipe(_request_data.data(), _request_data.size());
    _request = PartialRequest{};
    _descriptor_size = 0;
    _url_size = 0;
    _selectors_size = 0;
    _request_size = 0;
    _active = false;
}

} // namespace UI::LXMF::NomadNet
