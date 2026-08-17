#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace UI::LXMF::NomadNet {

enum class BlockType { TEXT, HEADING, DIVIDER, TABLE, UNSUPPORTED };
enum class Alignment { LEFT, CENTER, RIGHT };
enum class FormFieldType : uint8_t { TEXT, PASSWORD, CHECKBOX, RADIO };

enum class TruncationReason : uint32_t {
    DOCUMENT_BYTES = 1 << 0,
    SOURCE_LINES = 1 << 1,
    SOURCE_LINE_BYTES = 1 << 2,
    BLOCKS = 1 << 3,
    RUNS_PER_LINE = 1 << 4,
    TOTAL_RUNS = 1 << 5,
    LINKS = 1 << 6,
    ANCHORS = 1 << 7,
    ANCHOR_NAME_BYTES = 1 << 8,
    TABLES = 1 << 9,
    TABLE_ROWS = 1 << 10,
    TABLE_COLUMNS = 1 << 11,
    TABLE_CELL_BYTES = 1 << 12,
    TABLE_BYTES = 1 << 13,
    TABLE_CELLS = 1 << 14,
    TABLE_FALLBACK_BYTES = 1u << 15,
    FORM_FIELDS = 1u << 16,
    FORM_NAME_BYTES = 1u << 17,
    FORM_VALUE_BYTES = 1u << 18,
    FORM_LABEL_BYTES = 1u << 19,
    FORM_BYTES = 1u << 20,
};

struct Run {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool has_foreground = false;
    uint32_t foreground = 0;
    bool has_background = false;
    uint32_t background = 0;
    int link_index = -1;
    int field_index = -1;
};

struct FormField {
    uint16_t id = 0;
    FormFieldType type = FormFieldType::TEXT;
    std::string name;
    std::string value;
    std::string label;
    uint16_t width = 24;
    bool checked = false;
    bool masked = false;
};

struct TableCell {
    uint32_t first_run = 0;
    uint16_t run_count = 0;
    Alignment alignment = Alignment::LEFT;
};

struct Table {
    uint32_t first_cell = 0;
    uint16_t row_count = 0;
    uint8_t column_count = 0;
    Alignment alignment = Alignment::LEFT;
    uint16_t max_width = 100;
};

struct Block {
    BlockType type = BlockType::TEXT;
    uint8_t depth = 0;
    Alignment alignment = Alignment::LEFT;
    uint32_t divider_codepoint = 0x2500;
    int16_t table_index = -1;
    std::vector<Run> runs;
};

struct Link {
    Link() = default;
    Link(const std::string& link_label, const std::string& link_target,
         const std::string& link_fields, bool contains_fields = false)
        : label(link_label), target(link_target), fields(link_fields), has_fields(contains_fields) {}

    std::string label;
    std::string target;
    std::string fields;
    bool has_fields = false;
};

struct Anchor {
    Anchor() = default;
    Anchor(const std::string& value, uint16_t block)
        : name(value), block_index(block) {}

    std::string name;
    uint16_t block_index = 0;
};

struct Document {
    std::vector<Block> blocks;
    std::vector<Link> links;
    std::vector<Anchor> anchors;
    std::vector<Table> tables;
    std::vector<TableCell> table_cells;
    std::vector<Run> table_runs;
    std::vector<FormField> fields;
    uint32_t cache_seconds = 0;
    bool has_background = false;
    uint32_t background = 0;
    bool has_foreground = false;
    uint32_t foreground = 0;
    bool truncated = false;
    bool malformed = false;
    bool unsupported = false;
    std::size_t source_bytes = 0;
    std::size_t source_lines = 0;
    uint32_t truncation_reasons = 0;
    std::size_t form_bytes = 0;

    void mark_truncated(TruncationReason reason) {
        truncated = true;
        truncation_reasons |= static_cast<uint32_t>(reason);
    }
    bool has_truncation(TruncationReason reason) const {
        return (truncation_reasons & static_cast<uint32_t>(reason)) != 0;
    }
};

class DocumentParser {
public:
    static constexpr std::size_t MAX_DOCUMENT_BYTES = 64 * 1024;
    static constexpr std::size_t MAX_SOURCE_LINES = 4096;
    static constexpr std::size_t MAX_BLOCKS = 1024;
    static constexpr std::size_t MAX_RUNS_PER_LINE = 128;
    static constexpr std::size_t MAX_TOTAL_RUNS = 1024;
    static constexpr std::size_t MAX_SOURCE_LINE_BYTES = 4096;
    static constexpr std::size_t MAX_LINKS = 128;
    static constexpr std::size_t MAX_ANCHORS = 128;
    static constexpr std::size_t MAX_ANCHOR_NAME_BYTES = 64;
    static constexpr std::size_t MAX_TABLES = 16;
    static constexpr std::size_t MAX_TABLE_ROWS = 32;
    static constexpr std::size_t MAX_TABLE_COLUMNS = 8;
    static constexpr std::size_t MAX_TABLE_CELLS = 256;
    static constexpr std::size_t MAX_TOTAL_TABLE_CELLS = 512;
    static constexpr std::size_t MAX_TABLE_CELL_BYTES = 512;
    static constexpr std::size_t MAX_TABLE_FALLBACK_BYTES = 1024;
    static constexpr std::size_t MAX_TABLE_BYTES = 16 * 1024;
    static constexpr std::size_t MAX_FIELDS = 64;
    static constexpr std::size_t MAX_FIELD_NAME_BYTES = 64;
    static constexpr std::size_t MAX_FIELD_VALUE_BYTES = 512;
    static constexpr std::size_t MAX_FIELD_LABEL_BYTES = 256;
    static constexpr std::size_t MAX_FORM_BYTES = 16 * 1024;
    static constexpr uint16_t DEFAULT_FIELD_WIDTH = 24;
    static constexpr uint16_t MAX_FIELD_WIDTH = 256;
    static constexpr uint16_t DEFAULT_TABLE_WIDTH = 100;
    static constexpr uint16_t MAX_TABLE_WIDTH = UINT16_MAX;
    static constexpr uint32_t MAX_CACHE_SECONDS = 7 * 24 * 60 * 60;

    Document parse(const std::string& source) const;
    Document parse(const char* source, std::size_t size) const;
};

std::string truncation_notice(const Document& document);

} // namespace UI::LXMF::NomadNet
