#include "NomadNetDocument.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace UI::LXMF::NomadNet {
namespace {

bool valid_utf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const uint8_t lead = static_cast<uint8_t>(value[i]);
        if (lead <= 0x7f) { ++i; continue; }
        std::size_t continuation = 0;
        uint32_t codepoint = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { continuation = 1; codepoint = lead & 0x1f; }
        else if (lead >= 0xe0 && lead <= 0xef) { continuation = 2; codepoint = lead & 0x0f; }
        else if (lead >= 0xf0 && lead <= 0xf4) { continuation = 3; codepoint = lead & 0x07; }
        else return false;
        if (i + continuation >= value.size()) return false;
        for (std::size_t j = 1; j <= continuation; ++j) {
            const uint8_t byte = static_cast<uint8_t>(value[i + j]);
            if ((byte & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) return false;
        i += continuation + 1;
    }
    return true;
}

bool color(const std::string& value, uint32_t& result) {
    if (value.size() != 3 && value.size() != 6) return false;
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); })) return false;
    char* end = nullptr;
    result = static_cast<uint32_t>(std::strtoul(value.c_str(), &end, 16));
    if (value.size() == 3) {
        const uint32_t r = (result >> 8) & 0xf, g = (result >> 4) & 0xf, b = result & 0xf;
        result = (r * 17 << 16) | (g * 17 << 8) | b * 17;
    }
    return end && *end == '\0';
}

struct Style {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool has_foreground = false;
    uint32_t foreground = 0;
    bool has_background = false;
    uint32_t background = 0;
    Alignment alignment = Alignment::LEFT;
};

void add_run(Document& doc, Block& block, std::string& text, const Style& style, int link = -1) {
    if (text.empty()) return;
    if (block.runs.size() >= DocumentParser::MAX_RUNS_PER_LINE) {
        doc.truncated = true;
        text.clear();
        return;
    }
    Run run;
    run.text.swap(text);
    run.bold = style.bold;
    run.italic = style.italic;
    run.underline = style.underline;
    run.has_foreground = style.has_foreground;
    run.foreground = style.foreground;
    run.has_background = style.has_background;
    run.background = style.background;
    run.link_index = link;
    block.runs.push_back(std::move(run));
}

void parse_inline(Document& doc, Block& block, const std::string& line, Style& style) {
    std::string text;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            text.push_back(line[i + 1]); i += 2; continue;
        }
        if (line[i] != '`') { text.push_back(line[i++]); continue; }
        if (i + 1 >= line.size()) { text.push_back('`'); ++i; continue; }
        add_run(doc, block, text, style);
        const char command = line[i + 1];
        i += 2;
        if (command == '!') style.bold = !style.bold;
        else if (command == '*') style.italic = !style.italic;
        else if (command == '_') style.underline = !style.underline;
        else if (command == 'c') style.alignment = Alignment::CENTER;
        else if (command == 'l') style.alignment = Alignment::LEFT;
        else if (command == 'r') style.alignment = Alignment::RIGHT;
        else if (command == 'a') style.alignment = Alignment::LEFT;
        else if (command == 'f') style.has_foreground = false;
        else if (command == 'b') style.has_background = false;
        else if (command == 'F') {
            std::size_t count = 3;
            if (i < line.size() && line[i] == 'T') { ++i; count = 6; }
            if (i + count <= line.size()) {
                uint32_t value = 0;
                if (color(line.substr(i, count), value)) {
                    style.has_foreground = true;
                    style.foreground = value;
                    i += count;
                } else doc.malformed = true;
            } else doc.malformed = true;
        } else if (command == 'B') {
            std::size_t count = 3;
            if (i < line.size() && line[i] == 'T') { ++i; count = 6; }
            if (i + count <= line.size()) {
                uint32_t value = 0;
                if (color(line.substr(i, count), value)) {
                    style.has_background = true;
                    style.background = value;
                    i += count;
                } else doc.malformed = true;
            } else doc.malformed = true;
        } else if (command == '`') {
            style = Style{};
        } else if (command == '[') {
            const auto close = line.find(']', i);
            if (close == std::string::npos) { doc.malformed = true; text += "`["; continue; }
            const std::string value = line.substr(i, close - i);
            const auto separator = value.find('`');
            std::string label = separator == std::string::npos ? value : value.substr(0, separator);
            const auto fields_separator = separator == std::string::npos
                ? std::string::npos : value.find('`', separator + 1);
            std::string target = separator == std::string::npos ? value :
                value.substr(separator + 1, fields_separator == std::string::npos
                    ? std::string::npos : fields_separator - separator - 1);
            std::string fields = fields_separator == std::string::npos
                ? std::string() : value.substr(fields_separator + 1);
            if (target.empty()) { doc.malformed = true; }
            else if (doc.links.size() < DocumentParser::MAX_LINKS) {
                if (label.empty()) label = target;
                doc.links.push_back({label, target, fields});
                std::string link_text = label;
                add_run(doc, block, link_text, style, static_cast<int>(doc.links.size() - 1));
            } else doc.truncated = true;
            i = close + 1;
        } else {
            // Unknown modifiers are harmless and visible rather than commands.
            text.push_back('`'); text.push_back(command);
        }
    }
    add_run(doc, block, text, style);
    block.alignment = style.alignment;
}

} // namespace

Document DocumentParser::parse(const std::string& source) const {
    return parse(source.data(), source.size());
}

Document DocumentParser::parse(const char* source, std::size_t size) const {
    Document doc;
    if (!source && size != 0) {
        doc.malformed = true;
        return doc;
    }
    std::size_t retained = std::min(size, MAX_DOCUMENT_BYTES);
    while (retained > 0 && retained < size &&
           (static_cast<uint8_t>(source[retained]) & 0xc0) == 0x80) --retained;
    doc.source_bytes = retained;
    doc.truncated = size > retained;
    // Own an exact retained prefix so no find/substr operation can inspect
    // attacker-controlled bytes beyond the documented source cap.
    const std::string input(source ? source : "", retained);
    if (!valid_utf8(input)) {
        doc.malformed = true;
        return doc;
    }
    Style style;
    bool literal = false;
    uint8_t section_depth = 0;
    std::size_t total_runs = 0;
    std::size_t offset = 0;
    while (offset <= retained && doc.source_lines < MAX_SOURCE_LINES) {
        std::size_t end = input.find('\n', offset);
        if (end == std::string::npos || end > retained) end = retained;
        std::size_t length = end - offset;
        if (length > MAX_SOURCE_LINE_BYTES) {
            length = MAX_SOURCE_LINE_BYTES;
            while (length > 0 && offset + length < input.size() &&
                   (static_cast<uint8_t>(input[offset + length]) & 0xc0) == 0x80) --length;
            doc.truncated = true;
        }
        std::string line = input.substr(offset, length);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool first = doc.source_lines == 0;
        ++doc.source_lines;
        offset = end == retained ? retained + 1 : end + 1;

        if (first && line.rfind("#!c=", 0) == 0) {
            const std::string number = line.substr(4);
            if (!number.empty() && std::all_of(number.begin(), number.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
                char* parse_end = nullptr;
                const unsigned long long parsed = std::strtoull(number.c_str(), &parse_end, 10);
                if (parse_end && *parse_end == '\0') {
                    doc.cache_seconds = static_cast<uint32_t>(
                        std::min<unsigned long long>(parsed, MAX_CACHE_SECONDS));
                } else doc.malformed = true;
            } else doc.malformed = true;
            continue;
        }
        if (line.rfind("#!bg=", 0) == 0) {
            uint32_t value = 0;
            if (color(line.substr(5), value)) { doc.has_background = true; doc.background = value; }
            else doc.malformed = true;
            continue;
        }
        if (line.rfind("#!fg=", 0) == 0) {
            uint32_t value = 0;
            if (color(line.substr(5), value)) { doc.has_foreground = true; doc.foreground = value; }
            else doc.malformed = true;
            continue;
        }
        if (line == "`=") { literal = !literal; continue; }
        if (line.empty()) {
            if (doc.blocks.size() >= MAX_BLOCKS) { doc.truncated = true; break; }
            Block blank;
            blank.depth = section_depth;
            Run run;
            run.text = " ";
            blank.runs.push_back(std::move(run));
            doc.blocks.push_back(std::move(blank));
            continue;
        }
        while (!line.empty() && line[0] == '<') {
            section_depth = 0;
            line.erase(0, 1);
        }
        if (line.empty()) continue;
        if (doc.blocks.size() >= MAX_BLOCKS) { doc.truncated = true; break; }

        Block block;
        block.depth = section_depth;
        if (literal) {
            if (line == "\\`=") line = "`=";
            Run run;
            run.text = line;
            block.runs.push_back(std::move(run));
        } else if (line[0] == '#') {
            continue;
        } else if (line[0] == '>') {
            block.type = BlockType::HEADING;
            std::size_t depth = 0;
            while (depth < line.size() && line[depth] == '>') ++depth;
            block.depth = static_cast<uint8_t>(std::min<std::size_t>(depth, 255));
            section_depth = block.depth;
            parse_inline(doc, block, line.substr(depth), style);
        } else if (line[0] == '-') {
            block.type = BlockType::DIVIDER;
        } else if (line.rfind("`t", 0) == 0 || line.rfind("`{", 0) == 0 || line.find("`<") != std::string::npos) {
            block.type = BlockType::UNSUPPORTED;
            Run run;
            run.text = "[Unsupported Micron content]";
            block.runs.push_back(std::move(run));
            doc.unsupported = true;
        } else {
            parse_inline(doc, block, line, style);
        }
        if (total_runs + block.runs.size() > MAX_TOTAL_RUNS) {
            block.runs.resize(MAX_TOTAL_RUNS - total_runs);
            doc.truncated = true;
        }
        total_runs += block.runs.size();
        doc.blocks.push_back(std::move(block));
        if (total_runs >= MAX_TOTAL_RUNS) break;
    }
    if (offset <= retained) doc.truncated = true;
    if (literal) doc.malformed = true;
    return doc;
}

} // namespace UI::LXMF::NomadNet
