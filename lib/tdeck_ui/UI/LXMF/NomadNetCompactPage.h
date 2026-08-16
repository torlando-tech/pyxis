#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "NomadNetDocument.h"
#include "NomadNetMemory.h"

namespace UI::LXMF::NomadNet {

inline bool layout_content_truncated(std::size_t fragment_count,
                                     std::size_t fragment_limit,
                                     bool content_remaining) {
    return content_remaining && fragment_count >= fragment_limit;
}

inline bool block_has_layout_content(BlockType type, uint16_t run_count) {
    return type == BlockType::DIVIDER || type == BlockType::HEADING ||
           type == BlockType::TABLE || run_count != 0;
}

enum class TableLayoutTier : uint8_t { FIT, REFLOW };

inline TableLayoutTier choose_table_layout(int32_t structural_minimum_width,
                                           int32_t content_width) {
    return structural_minimum_width <= content_width
        ? TableLayoutTier::FIT : TableLayoutTier::REFLOW;
}

inline int16_t fit_table_columns(int16_t* widths, uint8_t column_count,
                                 int16_t minimum_width, int16_t target_width) {
    if (!widths || column_count == 0 ||
        column_count > DocumentParser::MAX_TABLE_COLUMNS || minimum_width <= 0)
        return 0;
    int32_t natural_width = 0;
    for (uint8_t column = 0; column < column_count; ++column) {
        widths[column] = std::max<int16_t>(minimum_width, widths[column]);
        natural_width += widths[column];
    }
    const int32_t structural_minimum = static_cast<int32_t>(minimum_width) * column_count;
    if (structural_minimum > INT16_MAX) return 0;
    const int32_t bounded_target = std::max<int32_t>(
        structural_minimum, std::min<int32_t>(target_width, INT16_MAX));
    if (natural_width <= bounded_target) return static_cast<int16_t>(natural_width);

    uint8_t order[DocumentParser::MAX_TABLE_COLUMNS] = {0};
    for (uint8_t column = 0; column < column_count; ++column) {
        uint8_t position = column;
        while (position > 0 && widths[order[position - 1]] < widths[column]) {
            order[position] = order[position - 1];
            --position;
        }
        order[position] = column;
    }
    int32_t excess = natural_width - bounded_target;
    for (uint8_t position = 0; position < column_count && excess > 0; ++position) {
        const uint8_t column = order[position];
        const int32_t reduction = std::min<int32_t>(
            excess, widths[column] - minimum_width);
        widths[column] = static_cast<int16_t>(widths[column] - reduction);
        excess -= reduction;
    }
    int32_t fitted_width = 0;
    for (uint8_t column = 0; column < column_count; ++column)
        fitted_width += widths[column];
    return static_cast<int16_t>(fitted_width);
}

inline uint8_t heading_display_level(uint8_t depth) {
    return depth <= 1 ? 1 : depth == 2 ? 2 : 3;
}

inline bool heading_uses_large_font(uint8_t depth) {
    return heading_display_level(depth) == 1;
}

inline uint8_t heading_indent_spaces(uint8_t depth) {
    if (depth <= 1) return 0;
    const uint8_t bounded_levels = depth > 8 ? 7 : static_cast<uint8_t>(depth - 1);
    return static_cast<uint8_t>(bounded_levels * 2);
}

inline uint8_t heading_bottom_spacing(uint8_t depth) {
    const uint8_t level = heading_display_level(depth);
    return level == 1 ? 6 : level == 2 ? 4 : 3;
}

class CompactPage {
public:
    static constexpr std::size_t MAX_BLOCKS = DocumentParser::MAX_BLOCKS;
    static constexpr std::size_t MAX_RUNS = DocumentParser::MAX_TOTAL_RUNS;
    static constexpr std::size_t MAX_LINKS = DocumentParser::MAX_LINKS;
    static constexpr std::size_t MAX_ANCHORS = DocumentParser::MAX_ANCHORS;
    static constexpr std::size_t MAX_TABLES = DocumentParser::MAX_TABLES;
    static constexpr std::size_t MAX_TABLE_CELLS = DocumentParser::MAX_TOTAL_TABLE_CELLS;
    // Text runs, link targets, and anchor names originate in the bounded source.
    // Link targets also remain visible in runs, while anchor declarations are
    // zero-width. Account for both bounded copies and one terminator per record.
    static constexpr std::size_t MAX_NOTICE_BYTES = 96;
    static constexpr std::size_t MAX_ARENA_BYTES =
        DocumentParser::MAX_DOCUMENT_BYTES * 2 +
        MAX_ANCHORS * (DocumentParser::MAX_ANCHOR_NAME_BYTES + 1) +
        MAX_RUNS + MAX_LINKS + MAX_NOTICE_BYTES + 1;

    enum Style : uint8_t {
        BOLD = 1 << 0,
        ITALIC = 1 << 1,
        UNDERLINE = 1 << 2,
        HAS_FOREGROUND = 1 << 3,
        HAS_BACKGROUND = 1 << 4,
    };

    struct BlockRecord {
        uint32_t first_run = 0;
        uint16_t run_count = 0;
        BlockType type = BlockType::TEXT;
        uint8_t depth = 0;
        Alignment alignment = Alignment::LEFT;
        uint32_t divider_codepoint = 0x2500;
        int16_t table_index = -1;
    };

    struct RunRecord {
        uint32_t text_offset = 0;
        uint16_t text_length = 0;
        int16_t link_index = -1;
        uint8_t style = 0;
        uint32_t foreground = 0;
        uint32_t background = 0;
    };

    struct LinkRecord {
        uint32_t target_offset = 0;
        uint16_t target_length = 0;
    };

    struct AnchorRecord {
        uint32_t name_offset = 0;
        uint16_t name_length = 0;
        uint16_t block_index = 0;
    };

    struct TableRecord {
        uint32_t first_cell = 0;
        uint16_t row_count = 0;
        uint8_t column_count = 0;
        Alignment alignment = Alignment::LEFT;
        uint16_t max_width = DocumentParser::DEFAULT_TABLE_WIDTH;
    };

    struct TableCellRecord {
        uint32_t first_run = 0;
        uint16_t run_count = 0;
        Alignment alignment = Alignment::LEFT;
    };

    struct TextView {
        const char* value = nullptr;
        std::size_t length = 0;
        TextView() = default;
        TextView(const char* data, std::size_t size) : value(data), length(size) {}
        const char* data() const { return value; }
        std::size_t size() const { return length; }
        bool empty() const { return length == 0; }
        char operator[](std::size_t index) const { return value[index]; }
    };

    bool assign(const Document& document);
    void clear();

    bool empty() const { return _blocks.empty(); }
    std::size_t arena_bytes() const { return _arena.size(); }
    const ExternalVector<BlockRecord>& blocks() const { return _blocks; }
    const ExternalVector<RunRecord>& runs() const { return _runs; }
    const ExternalVector<LinkRecord>& links() const { return _links; }
    const ExternalVector<AnchorRecord>& anchors() const { return _anchors; }
    const ExternalVector<TableRecord>& tables() const { return _tables; }
    const ExternalVector<TableCellRecord>& table_cells() const { return _table_cells; }
    TextView text(const RunRecord& run) const;
    TextView target(std::size_t index) const;
    bool find_anchor(const std::string& name, uint16_t& block_index) const;

    bool has_background() const { return _has_background; }
    uint32_t background() const { return _background; }
    bool has_foreground() const { return _has_foreground; }
    uint32_t foreground() const { return _foreground; }
    bool truncated() const { return _truncated; }
    bool unsupported() const { return _unsupported; }
    bool append_notice(const std::string& value);

private:
    bool append(const std::string& value, uint32_t& offset, uint16_t& length);
    bool append_display(const std::string& value, uint32_t& offset, uint16_t& length);

    ExternalVector<char> _arena;
    ExternalVector<BlockRecord> _blocks;
    ExternalVector<RunRecord> _runs;
    ExternalVector<LinkRecord> _links;
    ExternalVector<AnchorRecord> _anchors;
    ExternalVector<TableRecord> _tables;
    ExternalVector<TableCellRecord> _table_cells;
    bool _has_background = false;
    uint32_t _background = 0;
    bool _has_foreground = false;
    uint32_t _foreground = 0;
    bool _truncated = false;
    bool _unsupported = false;
};

} // namespace UI::LXMF::NomadNet
