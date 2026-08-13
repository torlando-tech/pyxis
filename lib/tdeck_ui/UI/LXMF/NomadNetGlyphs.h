#pragma once

#include <string>
#include <cstddef>

#ifdef ARDUINO
#include <lvgl.h>
LV_FONT_DECLARE(nomadnet_font_12)
LV_FONT_DECLARE(nomadnet_font_16)
#endif

namespace UI::LXMF::NomadNet {

// The bounded browser fonts contain printable ASCII, all glyphs in
// U+00A0-U+017F, and selected available glyphs from U+2000-U+206F,
// U+20A0-U+20CF, U+2190-U+21FF, and ten exact box/block glyphs observed on
// live NomadNet pages. Other valid Unicode is replaced at the display boundary
// so LVGL never draws its missing-glyph rectangle.
std::string display_text(const std::string& utf8);
using DisplayTextVisitor = void (*)(const char* bytes, std::size_t length, void* context);
void visit_display_text(const std::string& utf8, DisplayTextVisitor visitor, void* context);

} // namespace UI::LXMF::NomadNet
