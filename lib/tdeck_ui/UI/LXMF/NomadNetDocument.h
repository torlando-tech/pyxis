#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace UI::LXMF::NomadNet {

enum class BlockType { TEXT, HEADING, DIVIDER, UNSUPPORTED };
enum class Alignment { LEFT, CENTER, RIGHT };

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
};

struct Block {
    BlockType type = BlockType::TEXT;
    uint8_t depth = 0;
    Alignment alignment = Alignment::LEFT;
    std::vector<Run> runs;
};

struct Link {
    std::string label;
    std::string target;
    std::string fields;
};

struct Document {
    std::vector<Block> blocks;
    std::vector<Link> links;
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
    static constexpr uint32_t MAX_CACHE_SECONDS = 7 * 24 * 60 * 60;

    Document parse(const std::string& source) const;
    Document parse(const char* source, std::size_t size) const;
};

} // namespace UI::LXMF::NomadNet
