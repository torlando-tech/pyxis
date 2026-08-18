#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "NomadNetCompactPage.h"
#include "NomadNetMemory.h"

namespace UI::LXMF::NomadNet {

enum class FormEncodeResult : uint8_t {
    OK,
    INVALID_SELECTOR,
    VALUE_TOO_LARGE,
    TOO_MANY_ENTRIES,
    OUTPUT_TOO_LARGE,
    INVALID_STATE,
    ALLOCATION_FAILED,
};

inline void clear_encoded_form(ExternalVector<uint8_t>& bytes) {
    if (!bytes.empty()) {
        volatile uint8_t* cursor = bytes.data();
        for (std::size_t i = 0; i < bytes.size(); ++i) cursor[i] = 0;
    }
    ExternalVector<uint8_t>().swap(bytes);
}

class FormState {
public:
    static constexpr std::size_t MAX_SELECTOR_BYTES = 511;
    static constexpr std::size_t MAX_SELECTORS = 64;
    static constexpr std::size_t MAX_ENTRIES = DocumentParser::MAX_FIELDS + MAX_SELECTORS;
    // Keep the full request envelope below the pinned Link MDU (431 bytes),
    // avoiding Resource retention and bounding transient internal-SRAM copies.
    static constexpr std::size_t MAX_ENCODED_BYTES = 384;
    static constexpr std::size_t REQUEST_ENVELOPE_BUDGET = 32;
    static constexpr std::size_t PINNED_LINK_MDU = 431;
    static_assert(MAX_ENCODED_BYTES + REQUEST_ENVELOPE_BUDGET <= PINNED_LINK_MDU,
                  "Form requests must remain packet-sized");

    struct FieldState {
        uint16_t id = 0;
        FormFieldType type = FormFieldType::TEXT;
        std::array<char, DocumentParser::MAX_FIELD_NAME_BYTES + 1> name{};
        std::array<char, DocumentParser::MAX_FIELD_VALUE_BYTES + 1> value{};
        uint16_t name_length = 0;
        uint16_t value_length = 0;
        int16_t partial_region_index = -1;
        bool checked = false;
        bool masked = false;
        ~FieldState() {
            volatile char* bytes = value.data();
            for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
        }

        bool name_equals(const char* bytes, std::size_t size) const;
    };

    bool assign(const CompactPage& page);
    bool assign_preserving(const CompactPage& page, const FormState& previous);
    void clear();
    bool set_value(uint16_t id, const std::string& value);
    bool set_value(uint16_t id, const char* value, std::size_t size);
    bool set_checked(uint16_t id, bool checked);
    FormEncodeResult encode(const std::string& selectors,
                            ExternalVector<uint8_t>& output) const;

    const ExternalVector<FieldState>& fields() const { return _fields; }

private:
    ExternalVector<FieldState> _fields;
};

} // namespace UI::LXMF::NomadNet
