#include "NomadNetDocument.h"
#include "NomadNetPartialHash.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

bool first_codepoint(const std::string& value, std::size_t offset,
                     uint32_t& codepoint, std::size_t& length) {
    if (offset >= value.size()) return false;
    const uint8_t lead = static_cast<uint8_t>(value[offset]);
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
    } else return false;
    length = continuation + 1;
    if (offset + length > value.size()) return false;
    for (std::size_t i = 1; i <= continuation; ++i)
        codepoint = (codepoint << 6) |
            (static_cast<uint8_t>(value[offset + i]) & 0x3f);
    return true;
}

bool parse_micron_color(const std::string& value, uint32_t& result) {
    if (value.size() == 3 && value[0] == 'g') {
        if (!std::isdigit(static_cast<unsigned char>(value[1])) ||
            !std::isdigit(static_cast<unsigned char>(value[2]))) return false;
        const uint32_t percent = static_cast<uint32_t>(value[1] - '0') * 10 +
            static_cast<uint32_t>(value[2] - '0');
        // Urwid first scales gNN to 0..255, then chooses the nearest xterm
        // grayscale ramp entry. Preserve that quantization before RGB565.
        const uint32_t scaled = (percent * 255 * 2 + 100) / 200;
        static constexpr uint8_t levels[] = {
            0, 8, 18, 28, 38, 48, 58, 68, 78, 88, 98, 108, 118,
            128, 132, 148, 158, 168, 178, 188, 198, 208, 218, 228, 238, 255,
        };
        uint8_t gray = levels[sizeof(levels) - 1];
        for (std::size_t i = 0; i + 1 < sizeof(levels); ++i) {
            const uint32_t midpoint =
                (static_cast<uint32_t>(levels[i]) + levels[i + 1] + 1) / 2;
            if (scaled < midpoint) {
                gray = levels[i];
                break;
            }
        }
        result = static_cast<uint32_t>(gray) * 0x010101;
        return true;
    }
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

bool anchor_char(unsigned char c) {
    return c < 0x80 && (std::isalnum(c) != 0 || c == '_' || c == '-');
}

void add_anchor(Document& doc, const std::string& name, std::size_t block_index) {
    if (name.empty()) return;
    if (name.size() > DocumentParser::MAX_ANCHOR_NAME_BYTES) {
        doc.mark_truncated(TruncationReason::ANCHOR_NAME_BYTES);
        return;
    }
    for (const auto& anchor : doc.anchors)
        if (anchor.name == name) return;
    if (doc.anchors.size() >= DocumentParser::MAX_ANCHORS) {
        doc.mark_truncated(TruncationReason::ANCHORS);
        return;
    }
    doc.anchors.push_back({name, static_cast<uint16_t>(block_index)});
}

bool modifier_without_arguments(char command) {
    switch (command) {
        case '!': case '*': case '_': case '=': case 'f': case 'b':
        case 'a': case 'c': case 'r': case 'l': case '`': case '<':
        case '>': case '{': case '}': return true;
        default: return false;
    }
}

std::string heading_slug(const std::string& line) {
    std::string slug;
    slug.reserve(DocumentParser::MAX_ANCHOR_NAME_BYTES + 1);
    bool pending_separator = false;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '`' && i + 1 < line.size()) {
            const char command = line[i + 1];
            if (command == ':') {
                i += 2;
                while (i < line.size() && anchor_char(static_cast<unsigned char>(line[i]))) ++i;
                continue;
            }
            if (command == 'F' || command == 'B') {
                std::size_t value_start = i + 2;
                std::size_t count = 3;
                if (value_start < line.size() && line[value_start] == 'T') {
                    ++value_start;
                    count = 6;
                }
                uint32_t ignored = 0;
                if (value_start + count <= line.size() &&
                    parse_micron_color(line.substr(value_start, count), ignored)) {
                    i = value_start + count;
                    continue;
                }
            }
            if (modifier_without_arguments(command)) {
                i += 2;
                continue;
            }
        }

        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c < 0x80 && std::isalnum(c) != 0) {
            if (pending_separator && !slug.empty() &&
                slug.size() <= DocumentParser::MAX_ANCHOR_NAME_BYTES)
                slug.push_back('-');
            if (slug.size() <= DocumentParser::MAX_ANCHOR_NAME_BYTES)
                slug.push_back(static_cast<char>(std::tolower(c)));
            pending_separator = false;
            ++i;
        } else {
            pending_separator = !slug.empty();
            if (c < 0x80) ++i;
            else if (c <= 0xdf) i += 2;
            else if (c <= 0xef) i += 3;
            else i += 4;
        }
    }
    return slug;
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
        doc.mark_truncated(TruncationReason::RUNS_PER_LINE);
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

uint16_t form_width(const std::string& flags) {
    std::string digits;
    digits.reserve(flags.size());
    for (char c : flags)
        if (c != '^' && c != '?' && c != '!') digits.push_back(c);
    if (digits.empty()) return DocumentParser::DEFAULT_FIELD_WIDTH;
    char* end = nullptr;
    const long parsed = std::strtol(digits.c_str(), &end, 10);
    if (!end || end == digits.c_str() || *end != '\0') return DocumentParser::DEFAULT_FIELD_WIDTH;
    if (parsed <= 0) return 1;
    return static_cast<uint16_t>(std::min<long>(parsed, DocumentParser::MAX_FIELD_WIDTH));
}

std::vector<std::string> split_form_descriptor(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t separator = value.find('|', start);
        parts.push_back(value.substr(start, separator == std::string::npos
            ? std::string::npos : separator - start));
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return parts;
}

bool add_form_field(Document& doc, Block& block, const std::string& descriptor,
                    const std::string& data, const Style& style) {
    const auto parts = split_form_descriptor(descriptor);
    std::string flags;
    std::string name = descriptor;
    std::string submitted_value;
    bool prechecked = false;
    if (parts.size() > 1) {
        flags = parts[0];
        name = parts[1];
        if (parts.size() > 2) submitted_value = parts[2];
        if (parts.size() > 3) prechecked = parts[3] == "*";
    }

    FormFieldType type = FormFieldType::TEXT;
    bool masked = false;
    if (flags.find('^') != std::string::npos) type = FormFieldType::RADIO;
    else if (flags.find('?') != std::string::npos) type = FormFieldType::CHECKBOX;
    else if (flags.find('!') != std::string::npos) {
        type = FormFieldType::PASSWORD;
        masked = true;
    }
    const bool selection = type == FormFieldType::CHECKBOX || type == FormFieldType::RADIO;
    std::string value = selection ? (submitted_value.empty() ? data : submitted_value) : data;
    const std::string label = selection ? data : std::string();

    if (name.size() > DocumentParser::MAX_FIELD_NAME_BYTES) {
        doc.mark_truncated(TruncationReason::FORM_NAME_BYTES);
        return false;
    }
    if (value.size() > DocumentParser::MAX_FIELD_VALUE_BYTES) {
        doc.mark_truncated(TruncationReason::FORM_VALUE_BYTES);
        return false;
    }
    if (label.size() > DocumentParser::MAX_FIELD_LABEL_BYTES) {
        doc.mark_truncated(TruncationReason::FORM_LABEL_BYTES);
        return false;
    }
    if (doc.fields.size() >= DocumentParser::MAX_FIELDS) {
        doc.mark_truncated(TruncationReason::FORM_FIELDS);
        return false;
    }
    const std::size_t bytes = name.size() + value.size() + label.size();
    if (bytes > DocumentParser::MAX_FORM_BYTES -
                    std::min(doc.form_bytes, DocumentParser::MAX_FORM_BYTES)) {
        doc.mark_truncated(TruncationReason::FORM_BYTES);
        return false;
    }
    if (block.runs.size() >= DocumentParser::MAX_RUNS_PER_LINE) {
        doc.mark_truncated(TruncationReason::RUNS_PER_LINE);
        return false;
    }

    if (type == FormFieldType::RADIO && prechecked) {
        for (auto& existing : doc.fields)
            if (existing.type == FormFieldType::RADIO && existing.name == name)
                existing.checked = false;
    }
    FormField field;
    field.id = static_cast<uint16_t>(doc.fields.size());
    field.type = type;
    field.name = name;
    field.value = value;
    field.label = label;
    std::string width_flags = flags;
    width_flags.erase(std::remove_if(width_flags.begin(), width_flags.end(),
        [](char value) { return value == '^' || value == '?' || value == '!'; }),
        width_flags.end());
    field.width = selection ? DocumentParser::DEFAULT_FIELD_WIDTH : form_width(width_flags);
    field.checked = selection && prechecked;
    field.masked = masked;
    doc.form_bytes += bytes;
    doc.fields.push_back(std::move(field));

    Run placeholder;
    placeholder.bold = style.bold;
    placeholder.italic = style.italic;
    placeholder.underline = style.underline;
    placeholder.has_foreground = style.has_foreground;
    placeholder.foreground = style.foreground;
    placeholder.has_background = style.has_background;
    placeholder.background = style.background;
    placeholder.field_index = static_cast<int>(doc.fields.size() - 1);
    block.runs.push_back(std::move(placeholder));
    return true;
}

void parse_inline(Document& doc, Block& block, const std::string& line, Style& style) {
    std::string text;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            text.push_back(line[i + 1]); i += 2; continue;
        }
        if (line[i] != '`') { text.push_back(line[i++]); continue; }
        if (i + 1 >= line.size()) { ++i; continue; }
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
                if (parse_micron_color(line.substr(i, count), value)) {
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
                if (parse_micron_color(line.substr(i, count), value)) {
                    style.has_background = true;
                    style.background = value;
                    i += count;
                } else doc.malformed = true;
            } else doc.malformed = true;
        } else if (command == '`') {
            style = Style{};
        } else if (command == ':') {
            const std::size_t name_start = i;
            while (i < line.size() && anchor_char(static_cast<unsigned char>(line[i]))) ++i;
            add_anchor(doc, line.substr(name_start, i - name_start), doc.blocks.size());
        } else if (command == '<') {
            const std::size_t descriptor_end = line.find('`', i);
            if (descriptor_end == std::string::npos) {
                doc.malformed = true;
                continue;
            }
            const std::size_t field_end = line.find('>', descriptor_end + 1);
            if (field_end == std::string::npos) {
                doc.malformed = true;
                continue;
            }
            add_form_field(doc, block, line.substr(i, descriptor_end - i),
                           line.substr(descriptor_end + 1, field_end - descriptor_end - 1), style);
            i = field_end + 1;
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
            const bool has_third_component = fields_separator != std::string::npos;
            const bool has_fields = has_third_component && !fields.empty();
            const bool too_many_components = has_third_component &&
                value.find('`', fields_separator + 1) != std::string::npos;
            if (target.empty() || too_many_components) { doc.malformed = true; }
            else if (doc.links.size() < DocumentParser::MAX_LINKS) {
                if (label.empty()) label = target;
                doc.links.push_back({label, target, fields, has_fields});
                std::string link_text = label;
                add_run(doc, block, link_text, style, static_cast<int>(doc.links.size() - 1));
            } else doc.mark_truncated(TruncationReason::LINKS);
            i = close + 1;
        } else {
            const auto lead = static_cast<unsigned char>(command);
            const std::size_t continuation_count =
                lead >= 0xc2 && lead <= 0xdf ? 1 :
                lead >= 0xe0 && lead <= 0xef ? 2 :
                lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
            std::size_t consumed = 0;
            while (consumed < continuation_count && i < line.size()) {
                const auto next = static_cast<unsigned char>(line[i]);
                if ((next & 0xc0) != 0x80) break;
                ++i;
                ++consumed;
            }
        }
    }
    add_run(doc, block, text, style);
    block.alignment = style.alignment;
}

std::string trim_table_cell(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

struct BoundedTableRow {
    std::array<std::string, DocumentParser::MAX_TABLE_COLUMNS> cells;
    std::size_t count = 0;
    bool columns_exceeded = false;
    bool cell_bytes_exceeded = false;
};

BoundedTableRow parse_table_row(const std::string& source) {
    std::size_t first = 0;
    std::size_t last = source.size();
    while (first < last && std::isspace(static_cast<unsigned char>(source[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(source[last - 1]))) --last;
    if (first < last && source[first] == '|') ++first;
    if (last > first && source[last - 1] == '|') --last;

    BoundedTableRow row;
    std::string current;
    current.reserve(std::min<std::size_t>(last - first,
        DocumentParser::MAX_TABLE_CELL_BYTES));
    bool escaped = false;
    bool current_overflow_seen = false;
    auto append_cell_byte = [&](char value) {
        if (row.count >= DocumentParser::MAX_TABLE_COLUMNS) return;
        if (current_overflow_seen) {
            row.cell_bytes_exceeded = true;
            return;
        }
        if (current.size() < DocumentParser::MAX_TABLE_CELL_BYTES) {
            current.push_back(value);
            return;
        }
        row.cell_bytes_exceeded = true;
        if (!current_overflow_seen &&
            (static_cast<unsigned char>(value) & 0xc0) == 0x80) {
            while (!current.empty() &&
                   (static_cast<unsigned char>(current.back()) & 0xc0) == 0x80)
                current.pop_back();
            if (!current.empty() && static_cast<unsigned char>(current.back()) >= 0xc2)
                current.pop_back();
        }
        current_overflow_seen = true;
    };
    auto finish_cell = [&]() {
        if (row.count < DocumentParser::MAX_TABLE_COLUMNS) {
            row.cells[row.count++] = trim_table_cell(current);
        } else {
            row.columns_exceeded = true;
        }
        current.clear();
        current_overflow_seen = false;
    };
    for (std::size_t i = first; i < last; ++i) {
        const char value = source[i];
        if (escaped) {
            append_cell_byte(value);
            escaped = false;
        } else if (value == '\\') {
            escaped = true;
        } else if (value == '|') {
            finish_cell();
        } else {
            append_cell_byte(value);
        }
    }
    finish_cell();
    return row;
}

Alignment table_cell_alignment(const std::string& source) {
    const std::string value = trim_table_cell(source);
    if (!value.empty() && value.front() == ':' && value.back() == ':') return Alignment::CENTER;
    if (!value.empty() && value.back() == ':') return Alignment::RIGHT;
    return Alignment::LEFT;
}

void append_malformed_table(Document& doc, const std::vector<std::string>& lines,
                            std::size_t& total_runs) {
    if (doc.blocks.size() >= DocumentParser::MAX_BLOCKS) {
        doc.mark_truncated(TruncationReason::BLOCKS);
        return;
    }
    if (total_runs >= DocumentParser::MAX_TOTAL_RUNS) {
        doc.mark_truncated(TruncationReason::TOTAL_RUNS);
        return;
    }
    Block block;
    block.type = BlockType::UNSUPPORTED;
    Run run;
    run.text = "[Malformed table]";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string separator = i == 0 ? " " : " | ";
        const std::size_t remaining = DocumentParser::MAX_TABLE_FALLBACK_BYTES -
            std::min(run.text.size(), DocumentParser::MAX_TABLE_FALLBACK_BYTES);
        if (separator.size() + lines[i].size() <= remaining) {
            run.text += separator;
            run.text += lines[i];
            continue;
        }
        if (remaining > separator.size()) {
            run.text += separator;
            std::size_t retained = remaining - separator.size();
            retained = std::min(retained, lines[i].size());
            while (retained > 0 && retained < lines[i].size() &&
                   (static_cast<unsigned char>(lines[i][retained]) & 0xc0) == 0x80) --retained;
            run.text.append(lines[i], 0, retained);
        }
        doc.mark_truncated(TruncationReason::TABLE_FALLBACK_BYTES);
        break;
    }
    block.runs.push_back(std::move(run));
    doc.blocks.push_back(std::move(block));
    ++total_runs;
    doc.malformed = true;
}

bool append_table(Document& doc, const std::vector<std::string>& lines,
                  Alignment table_alignment, uint16_t max_width,
                  Style& style, std::size_t& total_runs) {
    if (lines.size() < 2) {
        append_malformed_table(doc, lines, total_runs);
        return false;
    }
    if (doc.tables.size() >= DocumentParser::MAX_TABLES) {
        doc.mark_truncated(TruncationReason::TABLES);
        return false;
    }
    if (doc.blocks.size() >= DocumentParser::MAX_BLOCKS) {
        doc.mark_truncated(TruncationReason::BLOCKS);
        return false;
    }

    const auto header = parse_table_row(lines.front());
    const auto alignment_cells = parse_table_row(lines[1]);
    const std::size_t columns = header.count;
    if (header.columns_exceeded || alignment_cells.columns_exceeded)
        doc.mark_truncated(TruncationReason::TABLE_COLUMNS);
    if (header.cell_bytes_exceeded || alignment_cells.cell_bytes_exceeded)
        doc.mark_truncated(TruncationReason::TABLE_CELL_BYTES);
    if (columns == 0) {
        append_malformed_table(doc, lines, total_runs);
        return false;
    }

    Table table;
    table.first_cell = static_cast<uint32_t>(doc.table_cells.size());
    table.column_count = static_cast<uint8_t>(columns);
    table.alignment = table_alignment;
    table.max_width = max_width;
    const std::size_t source_rows = lines.size() - 1;
    std::size_t rows = std::min(source_rows, DocumentParser::MAX_TABLE_ROWS);
    if (source_rows > rows) doc.mark_truncated(TruncationReason::TABLE_ROWS);
    const std::size_t document_cells_left = DocumentParser::MAX_TOTAL_TABLE_CELLS -
        std::min(doc.table_cells.size(), DocumentParser::MAX_TOTAL_TABLE_CELLS);
    const std::size_t table_cells_left = std::min(DocumentParser::MAX_TABLE_CELLS, document_cells_left);
    const std::size_t cell_bounded_rows = table_cells_left / columns;
    if (rows > cell_bounded_rows) {
        rows = cell_bounded_rows;
        doc.mark_truncated(TruncationReason::TABLE_CELLS);
    }
    if (rows == 0) return false;

    for (std::size_t row = 0; row < rows; ++row) {
        const auto cells = row == 0 ? header : parse_table_row(lines[row + 1]);
        if (cells.columns_exceeded) doc.mark_truncated(TruncationReason::TABLE_COLUMNS);
        if (cells.cell_bytes_exceeded) doc.mark_truncated(TruncationReason::TABLE_CELL_BYTES);
        for (std::size_t column = 0; column < columns; ++column) {
            TableCell cell;
            cell.first_run = static_cast<uint32_t>(doc.table_runs.size());
            cell.alignment = row == 0 ? Alignment::LEFT :
                (column < alignment_cells.count ? table_cell_alignment(alignment_cells.cells[column]) : Alignment::LEFT);
            Block parsed;
            if (column < cells.count) {
                std::string cell_source = cells.cells[column];
                const bool cell_truncated = cells.cell_bytes_exceeded &&
                    cell_source.size() == DocumentParser::MAX_TABLE_CELL_BYTES;
                const Style style_before_cell = style;
                if (cell_truncated) {
                    std::size_t retained = DocumentParser::MAX_TABLE_CELL_BYTES;
                    while (retained > 0 && retained < cell_source.size() &&
                           (static_cast<unsigned char>(cell_source[retained]) & 0xc0) == 0x80) --retained;
                    cell_source.resize(retained);
                    doc.mark_truncated(TruncationReason::TABLE_CELL_BYTES);
                }
                parse_inline(doc, parsed, cell_source, style);
                style = style_before_cell;
            }
            if (total_runs + parsed.runs.size() > DocumentParser::MAX_TOTAL_RUNS) {
                parsed.runs.resize(DocumentParser::MAX_TOTAL_RUNS - total_runs);
                doc.mark_truncated(TruncationReason::TOTAL_RUNS);
            }
            for (auto& run : parsed.runs) doc.table_runs.push_back(std::move(run));
            cell.run_count = static_cast<uint16_t>(doc.table_runs.size() - cell.first_run);
            total_runs += cell.run_count;
            doc.table_cells.push_back(cell);
        }
        ++table.row_count;
        if (total_runs >= DocumentParser::MAX_TOTAL_RUNS) break;
    }

    doc.tables.push_back(table);
    Block block;
    block.type = BlockType::TABLE;
    block.table_index = static_cast<int16_t>(doc.tables.size() - 1);
    block.alignment = table_alignment;
    doc.blocks.push_back(std::move(block));
    return true;
}

struct DecimalDigitRange {
    uint32_t first;
    uint32_t last;
};

constexpr DecimalDigitRange PYTHON_DECIMAL_DIGIT_RANGES[] = {
    {0x30U, 0x39U}, {0x660U, 0x669U}, {0x6F0U, 0x6F9U},
    {0x7C0U, 0x7C9U}, {0x966U, 0x96FU}, {0x9E6U, 0x9EFU},
    {0xA66U, 0xA6FU}, {0xAE6U, 0xAEFU}, {0xB66U, 0xB6FU},
    {0xBE6U, 0xBEFU}, {0xC66U, 0xC6FU}, {0xCE6U, 0xCEFU},
    {0xD66U, 0xD6FU}, {0xDE6U, 0xDEFU}, {0xE50U, 0xE59U},
    {0xED0U, 0xED9U}, {0xF20U, 0xF29U}, {0x1040U, 0x1049U},
    {0x1090U, 0x1099U}, {0x17E0U, 0x17E9U}, {0x1810U, 0x1819U},
    {0x1946U, 0x194FU}, {0x19D0U, 0x19D9U}, {0x1A80U, 0x1A89U},
    {0x1A90U, 0x1A99U}, {0x1B50U, 0x1B59U}, {0x1BB0U, 0x1BB9U},
    {0x1C40U, 0x1C49U}, {0x1C50U, 0x1C59U}, {0xA620U, 0xA629U},
    {0xA8D0U, 0xA8D9U}, {0xA900U, 0xA909U}, {0xA9D0U, 0xA9D9U},
    {0xA9F0U, 0xA9F9U}, {0xAA50U, 0xAA59U}, {0xABF0U, 0xABF9U},
    {0xFF10U, 0xFF19U}, {0x104A0U, 0x104A9U}, {0x10D30U, 0x10D39U},
    {0x11066U, 0x1106FU}, {0x110F0U, 0x110F9U}, {0x11136U, 0x1113FU},
    {0x111D0U, 0x111D9U}, {0x112F0U, 0x112F9U}, {0x11450U, 0x11459U},
    {0x114D0U, 0x114D9U}, {0x11650U, 0x11659U}, {0x116C0U, 0x116C9U},
    {0x11730U, 0x11739U}, {0x118E0U, 0x118E9U}, {0x11950U, 0x11959U},
    {0x11C50U, 0x11C59U}, {0x11D50U, 0x11D59U}, {0x11DA0U, 0x11DA9U},
    {0x11F50U, 0x11F59U}, {0x16A60U, 0x16A69U}, {0x16AC0U, 0x16AC9U},
    {0x16B50U, 0x16B59U}, {0x1D7CEU, 0x1D7FFU}, {0x1E140U, 0x1E149U},
    {0x1E2F0U, 0x1E2F9U}, {0x1E4F0U, 0x1E4F9U}, {0x1E950U, 0x1E959U},
    {0x1FBF0U, 0x1FBF9U},
};

bool decode_utf8_codepoint(const std::string& input, std::size_t& position,
                           std::size_t end, uint32_t& codepoint) {
    if (position >= end) return false;
    const unsigned char lead = static_cast<unsigned char>(input[position]);
    std::size_t length = 0;
    if (lead < 0x80U) {
        codepoint = lead;
        length = 1;
    } else if (lead >= 0xC2U && lead <= 0xDFU) {
        codepoint = lead & 0x1FU;
        length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        codepoint = lead & 0x0FU;
        length = 3;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        codepoint = lead & 0x07U;
        length = 4;
    } else {
        return false;
    }
    if (length > end - position) return false;
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char continuation = static_cast<unsigned char>(input[position + i]);
        if ((continuation & 0xC0U) != 0x80U) return false;
        codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU)
        return false;
    position += length;
    return true;
}

bool decode_python_decimal_digit(const std::string& input, std::size_t& position,
                                 std::size_t end, uint8_t& digit) {
    std::size_t after_codepoint = position;
    uint32_t codepoint = 0;
    if (!decode_utf8_codepoint(input, after_codepoint, end, codepoint)) return false;
    for (const auto& range : PYTHON_DECIMAL_DIGIT_RANGES) {
        if (codepoint >= range.first && codepoint <= range.last) {
            digit = static_cast<uint8_t>((codepoint - range.first) % 10U);
            position = after_codepoint;
            return true;
        }
    }
    return false;
}

bool python_float_whitespace(uint32_t codepoint) {
    return (codepoint >= 0x09U && codepoint <= 0x0DU) ||
           codepoint == 0x20U || codepoint == 0x85U || codepoint == 0xA0U ||
           codepoint == 0x1680U ||
           (codepoint >= 0x2000U && codepoint <= 0x200AU) ||
           codepoint == 0x2028U || codepoint == 0x2029U || codepoint == 0x202FU ||
           codepoint == 0x205FU || codepoint == 0x3000U;
}

bool parse_python_decimal(const std::string& input, double& value) {
    std::string canonical;
    canonical.reserve(input.size());
    for (std::size_t position = 0; position < input.size();) {
        std::size_t after_digit = position;
        uint8_t digit = 0;
        if (decode_python_decimal_digit(input, after_digit, input.size(), digit)) {
            canonical.push_back(static_cast<char>('0' + digit));
            position = after_digit;
            continue;
        }
        uint32_t codepoint = 0;
        std::size_t after_codepoint = position;
        if (!decode_utf8_codepoint(input, after_codepoint, input.size(), codepoint)) return false;
        if (python_float_whitespace(codepoint)) canonical.push_back(' ');
        else if (codepoint <= 0x7FU) canonical.push_back(static_cast<char>(codepoint));
        else return false;
        position = after_codepoint;
    }

    std::size_t begin = 0;
    std::size_t end = canonical.size();
    while (begin < end && canonical[begin] == ' ') ++begin;
    while (end > begin && canonical[end - 1] == ' ') --end;
    if (begin == end) return false;

    std::size_t cursor = begin;
    if (canonical[cursor] == '+' || canonical[cursor] == '-') ++cursor;
    if (cursor == end) return false;

    auto consume_digits = [&](std::size_t& position) {
        bool any = false;
        bool previous_digit = false;
        while (position < end) {
            const unsigned char current = static_cast<unsigned char>(canonical[position]);
            if (std::isdigit(current)) {
                any = true;
                previous_digit = true;
                ++position;
            } else if (canonical[position] == '_' && previous_digit &&
                       position + 1 < end) {
                if (!std::isdigit(static_cast<unsigned char>(canonical[position + 1]))) break;
                previous_digit = false;
                ++position;
            } else {
                break;
            }
        }
        return any && previous_digit;
    };

    const bool integer_digits = consume_digits(cursor);
    bool fraction_digits = false;
    if (cursor < end && canonical[cursor] == '.') {
        ++cursor;
        fraction_digits = consume_digits(cursor);
    }
    if (!integer_digits && !fraction_digits) return false;
    if (cursor < end && (canonical[cursor] == 'e' || canonical[cursor] == 'E')) {
        ++cursor;
        if (cursor < end && (canonical[cursor] == '+' || canonical[cursor] == '-')) ++cursor;
        if (!consume_digits(cursor)) return false;
    }
    if (cursor != end) return false;

    std::string normalized;
    normalized.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        if (canonical[i] != '_') normalized.push_back(canonical[i]);
    }
    char* parse_end = nullptr;
    value = std::strtod(normalized.c_str(), &parse_end);
    return parse_end && parse_end != normalized.c_str() && *parse_end == '\0';
}

bool parse_partial_descriptor(const std::string& line, Partial& partial,
                              TruncationReason& limit_reason) {
    limit_reason = TruncationReason::NONE;
    const std::size_t close = line.find('}', 2);
    if (line.rfind("`{", 0) != 0 || close == std::string::npos) return false;
    const std::size_t data_size = close - 2;
    if (data_size > DocumentParser::MAX_PARTIAL_DESCRIPTOR_BYTES) {
        limit_reason = TruncationReason::PARTIAL_DESCRIPTOR_BYTES;
        return false;
    }
    const std::string data = line.substr(2, data_size);
    const std::size_t first_separator = data.find('`');
    const std::size_t second_separator = first_separator == std::string::npos
        ? std::string::npos : data.find('`', first_separator + 1);
    if (second_separator != std::string::npos &&
        data.find('`', second_separator + 1) != std::string::npos) return false;

    const std::size_t url_end = first_separator == std::string::npos
        ? data.size() : first_separator;
    if (url_end == 0) return false;
    if (url_end > DocumentParser::MAX_PARTIAL_URL_BYTES) {
        limit_reason = TruncationReason::PARTIAL_DESCRIPTOR_BYTES;
        return false;
    }
    partial.url = data.substr(0, url_end);
    partial.descriptor = data;
    std::string hash_input = partial.url;

    if (first_separator != std::string::npos) {
        const std::size_t refresh_start = first_separator + 1;
        const std::size_t refresh_size = (second_separator == std::string::npos
            ? data.size() : second_separator) - refresh_start;
        const std::string refresh = data.substr(refresh_start, refresh_size);
        double seconds = 0.0;
        if (!parse_python_decimal(refresh, seconds) || !std::isfinite(seconds)) return false;
        if (seconds >= 1.0) {
            const double milliseconds = seconds * 1000.0;
            if (milliseconds > static_cast<double>(DocumentParser::MAX_PARTIAL_REFRESH_MS))
                return false;
            partial.refresh_interval_ms = static_cast<uint32_t>(milliseconds);
        }
        hash_input += "|" + refresh;
    }

    if (second_separator == std::string::npos) {
        // Canonical split(\"|\") semantics retain one empty selector.
        partial.fields.emplace_back();
    } else {
        const std::size_t selectors_start = second_separator + 1;
        const std::size_t selectors_size = data.size() - selectors_start;
        if (selectors_size > DocumentParser::MAX_PARTIAL_FIELD_BYTES) {
            limit_reason = TruncationReason::PARTIAL_FIELD_BYTES;
            return false;
        }
        partial.selectors = data.substr(selectors_start, selectors_size);
        hash_input += "|" + partial.selectors;
        std::size_t field_start = 0;
        while (field_start <= partial.selectors.size()) {
            if (partial.fields.size() >= DocumentParser::MAX_PARTIAL_FIELDS) {
                limit_reason = TruncationReason::PARTIAL_FIELDS;
                return false;
            }
            const std::size_t field_end = partial.selectors.find('|', field_start);
            const std::size_t field_size = (field_end == std::string::npos
                ? partial.selectors.size() : field_end) - field_start;
            if (field_size > DocumentParser::MAX_PARTIAL_FIELD_BYTES) {
                limit_reason = TruncationReason::PARTIAL_FIELD_BYTES;
                return false;
            }
            const std::string field = partial.selectors.substr(field_start, field_size);
            partial.fields.push_back(field);
            if (field.rfind("pid=", 0) == 0) {
                const std::size_t value_end = field.find('=', 4);
                const std::size_t id_size = (value_end == std::string::npos
                    ? field.size() : value_end) - 4;
                if (id_size > DocumentParser::MAX_PARTIAL_ID_BYTES) {
                    limit_reason = TruncationReason::PARTIAL_FIELD_BYTES;
                    return false;
                }
                partial.id = field.substr(4, id_size);
            }
            if (field_end == std::string::npos) break;
            field_start = field_end + 1;
        }
    }
    partial.descriptor_hash = partial_descriptor_sha256(
        hash_input.data(), hash_input.size());
    return true;
}

} // namespace

Document DocumentParser::parse(const std::string& source) const {
    return parse(source.data(), source.size());
}

ParseStatus DocumentParser::parse_into(const char* source, std::size_t size,
                                       Document& output) const noexcept {
    if (!source && size != 0) {
        output = Document{};
        output.malformed = true;
        return ParseStatus::INVALID_INPUT;
    }
    try {
        Document candidate = parse(source, size);
        output = std::move(candidate);
        return ParseStatus::OK;
    } catch (const std::bad_alloc&) {
        output = Document{};
        output.allocation_failed = true;
        return ParseStatus::ALLOCATION_FAILED;
    }
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
    if (size > retained) doc.mark_truncated(TruncationReason::DOCUMENT_BYTES);
    // Own an exact retained prefix so no find/substr operation can inspect
    // attacker-controlled bytes beyond the documented source cap.
    const std::string input(source ? source : "", retained);
    if (!valid_utf8(input)) {
        doc.malformed = true;
        return doc;
    }
    Style style;
    bool literal = false;
    bool table_mode = false;
    bool table_alignment_explicit = false;
    Alignment table_alignment = Alignment::LEFT;
    uint16_t table_width = DEFAULT_TABLE_WIDTH;
    std::size_t table_bytes = 0;
    std::size_t partial_bytes = 0;
    std::vector<std::string> table_lines;
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
            doc.mark_truncated(TruncationReason::SOURCE_LINE_BYTES);
        }
        std::string line = input.substr(offset, length);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool first = doc.source_lines == 0;
        ++doc.source_lines;
        offset = end == retained ? retained + 1 : end + 1;

        if (first && line.rfind("#!c=", 0) == 0) {
            doc.has_cache_directive = true;
            const std::string number = line.substr(4);
            if (!number.empty() && std::all_of(number.begin(), number.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
                char* parse_end = nullptr;
                const unsigned long long parsed = std::strtoull(number.c_str(), &parse_end, 10);
                if (parse_end && *parse_end == '\0') {
                    doc.cache_seconds = static_cast<uint32_t>(
                        std::min<unsigned long long>(parsed, MAX_CACHE_SECONDS));
                } else {
                    doc.cache_seconds = 0;
                    doc.malformed = true;
                    doc.cache_directive_valid = false;
                }
            } else {
                doc.cache_seconds = 0;
                doc.malformed = true;
                doc.cache_directive_valid = false;
            }
            continue;
        }
        if (!table_mode && line.rfind("#!bg=", 0) == 0) {
            uint32_t value = 0;
            if (parse_micron_color(line.substr(5), value)) { doc.has_background = true; doc.background = value; }
            else doc.malformed = true;
            continue;
        }
        if (!table_mode && line.rfind("#!fg=", 0) == 0) {
            uint32_t value = 0;
            if (parse_micron_color(line.substr(5), value)) { doc.has_foreground = true; doc.foreground = value; }
            else doc.malformed = true;
            continue;
        }
        if (line == "`=") { literal = !literal; continue; }
        if (line.empty()) {
            if (doc.blocks.size() >= MAX_BLOCKS) {
                doc.mark_truncated(TruncationReason::BLOCKS);
                break;
            }
            if (total_runs >= MAX_TOTAL_RUNS) {
                doc.mark_truncated(TruncationReason::TOTAL_RUNS);
                break;
            }
            Block blank;
            blank.depth = section_depth;
            Run run;
            run.text = " ";
            blank.runs.push_back(std::move(run));
            doc.blocks.push_back(std::move(blank));
            ++total_runs;
            continue;
        }
        if (line.empty()) continue;
        char classification_first_char = 0;
        while (!line.empty()) {
            if (!literal && line == "`=") {
                literal = true;
                line.clear();
                break;
            }
            classification_first_char = line[0];
            if (!literal && classification_first_char == '>' &&
                    line.find("`<") != std::string::npos) {
                const auto first_non_heading = line.find_first_not_of('>');
                line.erase(0, first_non_heading == std::string::npos
                    ? line.size() : first_non_heading);
                if (line.empty()) break;
                classification_first_char = line[0];
            }
            if (!literal && !table_mode && classification_first_char == '<') {
                section_depth = 0;
                line.erase(0, 1);
                continue;
            }
            break;
        }
        if (line.empty()) continue;
        bool pre_escaped = false;
        if (!literal && line[0] == '\\') {
            line.erase(0, 1);
            pre_escaped = true;
        }
        if (!literal && classification_first_char == '#') continue;
        if (!literal && line.rfind("`t", 0) == 0) {
            if (table_mode) {
                const bool canonical_table_rendered = table_lines.size() >= 2;
                append_table(doc, table_lines, table_alignment, table_width, style, total_runs);
                table_mode = false;
                table_lines.clear();
                table_bytes = 0;
                if (table_alignment_explicit && canonical_table_rendered)
                    style.alignment = Alignment::LEFT;
                table_alignment_explicit = false;
                table_alignment = style.alignment;
                table_width = DEFAULT_TABLE_WIDTH;
            } else {
                table_mode = true;
                table_lines.clear();
                table_bytes = 0;
                table_alignment_explicit = false;
                table_alignment = style.alignment;
                table_width = DEFAULT_TABLE_WIDTH;
                std::size_t option = 2;
                if (option < line.size() && (line[option] == 'l' || line[option] == 'c' || line[option] == 'r')) {
                    table_alignment_explicit = true;
                    table_alignment = line[option] == 'c' ? Alignment::CENTER :
                        line[option] == 'r' ? Alignment::RIGHT : Alignment::LEFT;
                    ++option;
                }
                if (option < line.size()) {
                    const char* width_start = line.c_str() + option;
                    char* width_end = nullptr;
                    const long long parsed = std::strtoll(width_start, &width_end, 10);
                    while (width_end && *width_end &&
                           std::isspace(static_cast<unsigned char>(*width_end))) ++width_end;
                    if (width_end && width_end != width_start && *width_end == '\0') {
                        if (parsed < 0) table_width = 1;
                        else if (parsed > 0) table_width = static_cast<uint16_t>(
                            std::min<unsigned long long>(
                                static_cast<unsigned long long>(parsed), MAX_TABLE_WIDTH));
                    }
                }
            }
            continue;
        }
        if (!literal && table_mode) {
            const std::size_t row_bytes = line.size() + 1;
            if (row_bytes <= MAX_TABLE_BYTES - std::min(table_bytes, MAX_TABLE_BYTES)) {
                table_lines.push_back(line);
                table_bytes += row_bytes;
            } else {
                doc.mark_truncated(TruncationReason::TABLE_BYTES);
            }
            continue;
        }
        if (doc.blocks.size() >= MAX_BLOCKS) {
            doc.mark_truncated(TruncationReason::BLOCKS);
            break;
        }

        Block block;
        block.depth = section_depth;
        if (literal) {
            if (line == "\\`=") line = "`=";
            Run run;
            run.text = line;
            block.runs.push_back(std::move(run));
        } else if (classification_first_char == '>') {
            block.type = BlockType::HEADING;
            std::size_t depth = 0;
            while (depth < line.size() && line[depth] == '>') ++depth;
            block.depth = static_cast<uint8_t>(std::min<std::size_t>(depth, 255));
            section_depth = block.depth;
            const std::string heading = line.substr(depth);
            parse_inline(doc, block, heading, style);
            add_anchor(doc, heading_slug(heading), doc.blocks.size());
        } else if (classification_first_char == '-') {
            block.type = BlockType::DIVIDER;
            uint32_t codepoint = 0;
            std::size_t codepoint_bytes = 0;
            if (first_codepoint(line, 1, codepoint, codepoint_bytes) &&
                line.size() == 1 + codepoint_bytes && codepoint >= 32) {
                block.divider_codepoint = codepoint;
            }
        } else if (line.rfind("`{", 0) == 0) {
            Partial partial;
            TruncationReason partial_limit = TruncationReason::NONE;
            if (parse_partial_descriptor(line, partial, partial_limit)) {
                bool admitted = true;
                if (doc.partials.size() >= MAX_PARTIALS) {
                    doc.mark_truncated(TruncationReason::PARTIALS);
                    admitted = false;
                }
                if (partial.descriptor.size() > MAX_PARTIAL_DESCRIPTOR_BYTES ||
                    partial.url.size() > MAX_PARTIAL_URL_BYTES) {
                    doc.mark_truncated(TruncationReason::PARTIAL_DESCRIPTOR_BYTES);
                    admitted = false;
                }
                if (partial.fields.size() > MAX_PARTIAL_FIELDS) {
                    doc.mark_truncated(TruncationReason::PARTIAL_FIELDS);
                    admitted = false;
                }
                if (partial.selectors.size() > MAX_PARTIAL_FIELD_BYTES ||
                    partial.id.size() > MAX_PARTIAL_ID_BYTES) {
                    doc.mark_truncated(TruncationReason::PARTIAL_FIELD_BYTES);
                    admitted = false;
                }
                for (const auto& field : partial.fields) {
                    if (field.size() > MAX_PARTIAL_FIELD_BYTES) {
                        doc.mark_truncated(TruncationReason::PARTIAL_FIELD_BYTES);
                        admitted = false;
                        break;
                    }
                }
                std::size_t bytes = partial.descriptor.size() + partial.url.size() +
                    partial.selectors.size() + partial.id.size() + partial.fields.size() + 4;
                for (const auto& field : partial.fields) bytes += field.size();
                if (bytes > MAX_PARTIAL_BYTES - std::min(partial_bytes, MAX_PARTIAL_BYTES)) {
                    doc.mark_truncated(TruncationReason::PARTIAL_DESCRIPTOR_BYTES);
                    admitted = false;
                }
                if (admitted) {
                    partial_bytes += bytes;
                    block.type = BlockType::PARTIAL;
                    doc.partials.push_back(std::move(partial));
                    block.partial_index = static_cast<int16_t>(doc.partials.size() - 1);
                    Run run;
                    run.text = "[Dynamic content loading]";
                    block.runs.push_back(std::move(run));
                } else {
                    block.type = BlockType::UNSUPPORTED;
                    Run run;
                    run.text = "[Partial omitted: limits exceeded]";
                    block.runs.push_back(std::move(run));
                }
            } else {
                block.type = BlockType::UNSUPPORTED;
                Run run;
                if (partial_limit != TruncationReason::NONE) {
                    doc.mark_truncated(partial_limit);
                    run.text = "[Partial omitted: limits exceeded]";
                } else {
                    run.text = "[Invalid Micron partial]";
                    doc.malformed = true;
                }
                block.runs.push_back(std::move(run));
            }
        } else {
            if (pre_escaped) line.insert(line.begin(), '\\');
            parse_inline(doc, block, line, style);
        }
        const bool total_runs_exceeded =
            total_runs + block.runs.size() > MAX_TOTAL_RUNS;
        if (total_runs_exceeded) {
            block.runs.resize(MAX_TOTAL_RUNS - total_runs);
            doc.mark_truncated(TruncationReason::TOTAL_RUNS);
        }
        total_runs += block.runs.size();
        doc.blocks.push_back(std::move(block));
        if (total_runs_exceeded) break;
    }
    if (offset <= retained && doc.source_lines >= MAX_SOURCE_LINES)
        doc.mark_truncated(TruncationReason::SOURCE_LINES);
    if (table_mode) append_malformed_table(doc, table_lines, total_runs);
    if (literal) doc.malformed = true;
    return doc;
}

std::string truncation_notice(const Document& document) {
    if (document.has_truncation(TruncationReason::DOCUMENT_BYTES))
        return "[Page truncated: source exceeds " +
            std::to_string(DocumentParser::MAX_DOCUMENT_BYTES / 1024) + " KiB]";
    if (document.has_truncation(TruncationReason::SOURCE_LINE_BYTES))
        return "[Page truncated: line exceeds " +
            std::to_string(DocumentParser::MAX_SOURCE_LINE_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::SOURCE_LINES))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_SOURCE_LINES) + " lines]";
    if (document.has_truncation(TruncationReason::BLOCKS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_BLOCKS) + " blocks]";
    if (document.has_truncation(TruncationReason::RUNS_PER_LINE))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_RUNS_PER_LINE) + " styles on one line]";
    if (document.has_truncation(TruncationReason::TOTAL_RUNS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_TOTAL_RUNS) + " styled runs]";
    if (document.has_truncation(TruncationReason::LINKS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_LINKS) + " links]";
    if (document.has_truncation(TruncationReason::ANCHOR_NAME_BYTES))
        return "[Page truncated: anchor name exceeds " +
            std::to_string(DocumentParser::MAX_ANCHOR_NAME_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::ANCHORS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_ANCHORS) + " anchors]";
    if (document.has_truncation(TruncationReason::TABLE_FALLBACK_BYTES))
        return "[Page truncated: malformed table fallback exceeds " +
            std::to_string(DocumentParser::MAX_TABLE_FALLBACK_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::TABLE_CELL_BYTES))
        return "[Page truncated: table cell exceeds " +
            std::to_string(DocumentParser::MAX_TABLE_CELL_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::TABLE_BYTES))
        return "[Page truncated: table exceeds " +
            std::to_string(DocumentParser::MAX_TABLE_BYTES / 1024) + " KiB]";
    if (document.has_truncation(TruncationReason::TABLE_COLUMNS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_TABLE_COLUMNS) + " table columns]";
    if (document.has_truncation(TruncationReason::TABLE_ROWS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_TABLE_ROWS) + " table rows]";
    if (document.has_truncation(TruncationReason::TABLE_CELLS))
        return "[Page truncated: too many table cells]";
    if (document.has_truncation(TruncationReason::TABLES))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_TABLES) + " tables]";
    if (document.has_truncation(TruncationReason::PARTIALS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_PARTIALS) + " dynamic partials]";
    if (document.has_truncation(TruncationReason::PARTIAL_DESCRIPTOR_BYTES))
        return "[Page truncated: dynamic partial metadata exceeds limits]";
    if (document.has_truncation(TruncationReason::PARTIAL_FIELDS))
        return "[Page truncated: too many dynamic partial fields]";
    if (document.has_truncation(TruncationReason::PARTIAL_FIELD_BYTES))
        return "[Page truncated: dynamic partial field exceeds limits]";
    if (document.has_truncation(TruncationReason::FORM_NAME_BYTES))
        return "[Page truncated: form field name exceeds " +
            std::to_string(DocumentParser::MAX_FIELD_NAME_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::FORM_VALUE_BYTES))
        return "[Page truncated: form field value exceeds " +
            std::to_string(DocumentParser::MAX_FIELD_VALUE_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::FORM_LABEL_BYTES))
        return "[Page truncated: form field label exceeds " +
            std::to_string(DocumentParser::MAX_FIELD_LABEL_BYTES) + " bytes]";
    if (document.has_truncation(TruncationReason::FORM_BYTES))
        return "[Page truncated: form data exceeds " +
            std::to_string(DocumentParser::MAX_FORM_BYTES / 1024) + " KiB]";
    if (document.has_truncation(TruncationReason::FORM_FIELDS))
        return "[Page truncated: more than " +
            std::to_string(DocumentParser::MAX_FIELDS) + " form fields]";
    return "[Page truncated to device safety limits]";
}

} // namespace UI::LXMF::NomadNet
