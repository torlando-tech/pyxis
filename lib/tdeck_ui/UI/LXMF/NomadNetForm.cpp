#include "NomadNetForm.h"

#include <algorithm>
#include <cstring>

namespace UI::LXMF::NomadNet {
namespace {

void wipe(char* bytes, std::size_t size) {
    volatile char* cursor = bytes;
    while (size-- != 0) *cursor++ = 0;
}

struct MapEntry {
    static constexpr std::size_t MAX_KEY_BYTES =
        sizeof("field_") - 1 + DocumentParser::MAX_FIELD_NAME_BYTES;
    std::array<char, MAX_KEY_BYTES + 1> key{};
    std::array<char, DocumentParser::MAX_FIELD_VALUE_BYTES + 1> value{};
    uint16_t key_length = 0;
    uint16_t value_length = 0;
    ~MapEntry() { wipe(value.data(), value.size()); }
};

bool same_key(const MapEntry& entry, const char* key, std::size_t key_length) {
    return entry.key_length == key_length &&
        std::memcmp(entry.key.data(), key, key_length) == 0;
}

FormEncodeResult upsert(ExternalVector<MapEntry>& entries,
                        const char* prefix, std::size_t prefix_length,
                        const char* name, std::size_t name_length,
                        const char* value, std::size_t value_length,
                        bool append_checkbox) {
    if (name_length > DocumentParser::MAX_FIELD_NAME_BYTES ||
        value_length > DocumentParser::MAX_FIELD_VALUE_BYTES)
        return FormEncodeResult::VALUE_TOO_LARGE;
    std::array<char, MapEntry::MAX_KEY_BYTES + 1> key{};
    const std::size_t key_length = prefix_length + name_length;
    if (key_length > MapEntry::MAX_KEY_BYTES) return FormEncodeResult::INVALID_SELECTOR;
    std::memcpy(key.data(), prefix, prefix_length);
    if (name_length != 0) std::memcpy(key.data() + prefix_length, name, name_length);

    for (auto& entry : entries) {
        if (!same_key(entry, key.data(), key_length)) continue;
        if (append_checkbox && entry.value_length != 0) {
            if (static_cast<std::size_t>(entry.value_length) + 1 + value_length >
                    DocumentParser::MAX_FIELD_VALUE_BYTES)
                return FormEncodeResult::VALUE_TOO_LARGE;
            entry.value[entry.value_length++] = ',';
            if (value_length != 0)
                std::memcpy(entry.value.data() + entry.value_length, value, value_length);
            entry.value_length = static_cast<uint16_t>(entry.value_length + value_length);
            entry.value[entry.value_length] = '\0';
        } else {
            wipe(entry.value.data(), entry.value.size());
            if (value_length != 0) std::memcpy(entry.value.data(), value, value_length);
            entry.value_length = static_cast<uint16_t>(value_length);
        }
        return FormEncodeResult::OK;
    }

    if (entries.size() >= FormState::MAX_ENTRIES)
        return FormEncodeResult::TOO_MANY_ENTRIES;
    MapEntry entry;
    if (key_length != 0) std::memcpy(entry.key.data(), key.data(), key_length);
    entry.key_length = static_cast<uint16_t>(key_length);
    if (value_length != 0) std::memcpy(entry.value.data(), value, value_length);
    entry.value_length = static_cast<uint16_t>(value_length);
    entries.push_back(std::move(entry));
    return FormEncodeResult::OK;
}

bool append_byte(ExternalVector<uint8_t>& output, uint8_t value) {
    if (output.size() >= FormState::MAX_ENCODED_BYTES) return false;
    output.push_back(value);
    return true;
}

bool append_bytes(ExternalVector<uint8_t>& output, const char* value, std::size_t size) {
    if (size > FormState::MAX_ENCODED_BYTES -
                   std::min(output.size(), FormState::MAX_ENCODED_BYTES)) return false;
    output.insert(output.end(), value, value + size);
    return true;
}

bool append_string(ExternalVector<uint8_t>& output, const char* value, std::size_t size) {
    if (size <= 31) {
        if (!append_byte(output, static_cast<uint8_t>(0xa0u | size))) return false;
    } else if (size <= 0xff) {
        if (!append_byte(output, 0xd9) || !append_byte(output, static_cast<uint8_t>(size))) return false;
    } else if (size <= 0xffff) {
        if (!append_byte(output, 0xda) ||
            !append_byte(output, static_cast<uint8_t>(size >> 8)) ||
            !append_byte(output, static_cast<uint8_t>(size))) return false;
    } else return false;
    return append_bytes(output, value, size);
}

bool selector_has_name(const std::string& selectors, const FormState::FieldState& field) {
    for (std::size_t start = 0; start <= selectors.size();) {
        const std::size_t end = selectors.find('|', start);
        const std::size_t size = (end == std::string::npos ? selectors.size() : end) - start;
        const std::size_t equals = selectors.find('=', start);
        const bool assignment = equals != std::string::npos && equals < start + size;
        if (!assignment && field.name_equals(selectors.data() + start, size)) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

} // namespace

bool FormState::FieldState::name_equals(const char* bytes, std::size_t size) const {
    return bytes && size == name_length && std::memcmp(name.data(), bytes, size) == 0;
}

bool FormState::assign(const CompactPage& page) {
    clear();
    if (page.fields().size() > DocumentParser::MAX_FIELDS) return false;
    try {
        _fields.reserve(page.fields().size());
        for (std::size_t i = 0; i < page.fields().size(); ++i) {
            const auto name = page.field_name(i);
            const auto value = page.field_value(i);
            if ((!name.data() && !name.empty()) || (!value.data() && !value.empty()) ||
                name.size() > DocumentParser::MAX_FIELD_NAME_BYTES ||
                value.size() > DocumentParser::MAX_FIELD_VALUE_BYTES) {
                clear();
                return false;
            }
            FieldState field;
            field.id = static_cast<uint16_t>(i);
            field.type = page.fields()[i].type;
            field.name_length = static_cast<uint16_t>(name.size());
            field.value_length = static_cast<uint16_t>(value.size());
            if (!name.empty()) std::memcpy(field.name.data(), name.data(), name.size());
            if (!value.empty()) std::memcpy(field.value.data(), value.data(), value.size());
            field.checked = page.fields()[i].checked;
            field.masked = page.fields()[i].masked;
            _fields.push_back(std::move(field));
        }
        return true;
    } catch (const std::bad_alloc&) {
        clear();
        return false;
    }
}

void FormState::clear() {
    for (auto& field : _fields) wipe(field.value.data(), field.value.size());
    ExternalVector<FieldState>().swap(_fields);
}

bool FormState::set_value(uint16_t id, const std::string& value) {
    return set_value(id, value.data(), value.size());
}

bool FormState::set_value(uint16_t id, const char* value, std::size_t size) {
    if (id >= _fields.size() || (!value && size != 0) ||
        size > DocumentParser::MAX_FIELD_VALUE_BYTES) return false;
    auto& field = _fields[id];
    if (field.type != FormFieldType::TEXT && field.type != FormFieldType::PASSWORD) return false;
    wipe(field.value.data(), field.value.size());
    if (size != 0) std::memcpy(field.value.data(), value, size);
    field.value_length = static_cast<uint16_t>(size);
    return true;
}

bool FormState::set_checked(uint16_t id, bool checked) {
    if (id >= _fields.size()) return false;
    auto& field = _fields[id];
    if (field.type != FormFieldType::CHECKBOX && field.type != FormFieldType::RADIO) return false;
    if (field.type == FormFieldType::RADIO && checked) {
        for (auto& existing : _fields)
            if (existing.type == FormFieldType::RADIO &&
                existing.name_length == field.name_length &&
                std::memcmp(existing.name.data(), field.name.data(), field.name_length) == 0)
                existing.checked = false;
    }
    field.checked = checked;
    return true;
}

FormEncodeResult FormState::encode(const std::string& selectors,
                                   ExternalVector<uint8_t>& output) const {
    clear_encoded_form(output);
    if (selectors.size() > MAX_SELECTOR_BYTES) return FormEncodeResult::INVALID_SELECTOR;

    try {
        if (selectors.empty()) {
            output.push_back(0x80);
            return FormEncodeResult::OK;
        }
        ExternalVector<MapEntry> entries;
        entries.reserve(std::min<std::size_t>(MAX_ENTRIES, _fields.size() + 4));
        bool all_fields = false;
        std::size_t selector_count = 0;

        for (std::size_t start = 0; start <= selectors.size();) {
            if (++selector_count > MAX_SELECTORS) return FormEncodeResult::INVALID_SELECTOR;
            const std::size_t end = selectors.find('|', start);
            const std::size_t size = (end == std::string::npos ? selectors.size() : end) - start;
            const char* segment = selectors.data() + start;
            if (size == 1 && segment[0] == '*') all_fields = true;
            const void* first_equals_ptr = std::memchr(segment, '=', size);
            if (first_equals_ptr) {
                const auto* first_equals = static_cast<const char*>(first_equals_ptr);
                const std::size_t name_size = static_cast<std::size_t>(first_equals - segment);
                const std::size_t value_size = size - name_size - 1;
                if (!std::memchr(first_equals + 1, '=', value_size)) {
                    const auto result = upsert(entries, "var_", 4, segment, name_size,
                                               first_equals + 1, value_size, false);
                    if (result != FormEncodeResult::OK) return result;
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }

        for (const auto& field : _fields) {
            if (!all_fields && !selector_has_name(selectors, field)) continue;
            if ((field.type == FormFieldType::CHECKBOX || field.type == FormFieldType::RADIO) &&
                !field.checked) continue;
            const bool append_checkbox = field.type == FormFieldType::CHECKBOX;
            const auto result = upsert(entries, "field_", 6,
                                       field.name.data(), field.name_length,
                                       field.value.data(), field.value_length,
                                       append_checkbox);
            if (result != FormEncodeResult::OK) return result;
        }

        if (entries.size() <= 15) {
            if (!append_byte(output, static_cast<uint8_t>(0x80u | entries.size())))
                return FormEncodeResult::OUTPUT_TOO_LARGE;
        } else {
            if (!append_byte(output, 0xde) ||
                !append_byte(output, static_cast<uint8_t>(entries.size() >> 8)) ||
                !append_byte(output, static_cast<uint8_t>(entries.size())))
                return FormEncodeResult::OUTPUT_TOO_LARGE;
        }
        for (const auto& entry : entries) {
            if (!append_string(output, entry.key.data(), entry.key_length) ||
                !append_string(output, entry.value.data(), entry.value_length)) {
                clear_encoded_form(output);
                return FormEncodeResult::OUTPUT_TOO_LARGE;
            }
        }
        return FormEncodeResult::OK;
    } catch (const std::bad_alloc&) {
        clear_encoded_form(output);
        return FormEncodeResult::OUTPUT_TOO_LARGE;
    }
}

} // namespace UI::LXMF::NomadNet
