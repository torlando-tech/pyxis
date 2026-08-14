#include "NomadNetGlyphs.h"
#include "../Fonts/NomadNetFontCoverage.h"

#include <cstddef>
#include <cstdint>

namespace UI::LXMF::NomadNet {
namespace {

bool decode_utf8(const std::string& value, std::size_t offset,
                 uint32_t& codepoint, std::size_t& length) {
    const auto lead = static_cast<uint8_t>(value[offset]);
    if (lead <= 0x7f) {
        codepoint = lead;
        length = 1;
        return true;
    }

    std::size_t continuation = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
        codepoint = lead & 0x1f;
        continuation = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        codepoint = lead & 0x0f;
        continuation = 2;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        codepoint = lead & 0x07;
        continuation = 3;
    } else {
        length = 1;
        return false;
    }

    length = continuation + 1;
    if (offset + length > value.size()) {
        length = 1;
        return false;
    }
    for (std::size_t i = 1; i <= continuation; ++i) {
        const auto byte = static_cast<uint8_t>(value[offset + i]);
        if ((byte & 0xc0) != 0x80) {
            length = 1;
            return false;
        }
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if ((continuation == 1 && codepoint < 0x80) ||
        (continuation == 2 && codepoint < 0x800) ||
        (continuation == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff) {
        length = 1;
        return false;
    }
    return true;
}

} // namespace

void visit_display_text(const std::string& utf8, DisplayTextVisitor visitor, void* context) {
    static const char replacement = '?';
    for (std::size_t offset = 0; offset < utf8.size();) {
        uint32_t codepoint = 0;
        std::size_t length = 1;
        if (!decode_utf8(utf8, offset, codepoint, length) ||
            !nomadnet_font_has_codepoint(codepoint)) {
            visitor(&replacement, 1, context);
        } else {
            visitor(utf8.data() + offset, length, context);
        }
        offset += length;
    }
}

std::string display_text(const std::string& utf8) {
    std::string output;
    output.reserve(utf8.size());
    visit_display_text(utf8, [](const char* bytes, std::size_t length, void* context) {
        static_cast<std::string*>(context)->append(bytes, length);
    }, &output);
    return output;
}

} // namespace UI::LXMF::NomadNet
