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
    return type == BlockType::DIVIDER || type == BlockType::HEADING || run_count != 0;
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
    // Text runs and link targets both originate in the bounded source, but a
    // link's target is also represented inside its visible run. Account for
    // that bounded duplication plus one terminator per retained record.
    static constexpr std::size_t MAX_NOTICE_BYTES = 96;
    static constexpr std::size_t MAX_ARENA_BYTES =
        DocumentParser::MAX_DOCUMENT_BYTES * 2 + MAX_RUNS + MAX_LINKS +
        MAX_NOTICE_BYTES + 1;

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
    TextView text(const RunRecord& run) const;
    TextView target(std::size_t index) const;

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
    bool _has_background = false;
    uint32_t _background = 0;
    bool _has_foreground = false;
    uint32_t _foreground = 0;
    bool _truncated = false;
    bool _unsupported = false;
};

} // namespace UI::LXMF::NomadNet
