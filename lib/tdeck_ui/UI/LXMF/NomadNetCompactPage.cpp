#include "NomadNetCompactPage.h"
#include "NomadNetGlyphs.h"

#include <algorithm>
#include <limits>

namespace UI::LXMF::NomadNet {

bool CompactPage::append(const std::string& value, uint32_t& offset, uint16_t& length) {
    if (value.size() > std::numeric_limits<uint16_t>::max()) return false;
    if (value.size() + 1 > MAX_ARENA_BYTES - std::min(_arena.size(), MAX_ARENA_BYTES)) return false;
    if (_arena.size() > std::numeric_limits<uint32_t>::max() - value.size() - 1) return false;
    offset = static_cast<uint32_t>(_arena.size());
    length = static_cast<uint16_t>(value.size());
    _arena.insert(_arena.end(), value.begin(), value.end());
    _arena.push_back('\0');
    return true;
}

bool CompactPage::append_display(const std::string& value, uint32_t& offset, uint16_t& length) {
    if (value.size() > std::numeric_limits<uint16_t>::max()) return false;
    if (value.size() + 1 > MAX_ARENA_BYTES - std::min(_arena.size(), MAX_ARENA_BYTES)) return false;
    offset = static_cast<uint32_t>(_arena.size());
    struct Context { ExternalVector<char>* arena; std::size_t bytes; } context{&_arena, 0};
    visit_display_text(value, [](const char* bytes, std::size_t count, void* opaque) {
        auto* state = static_cast<Context*>(opaque);
        state->arena->insert(state->arena->end(), bytes, bytes + count);
        state->bytes += count;
    }, &context);
    if (context.bytes > std::numeric_limits<uint16_t>::max()) return false;
    length = static_cast<uint16_t>(context.bytes);
    _arena.push_back('\0');
    return true;
}

bool CompactPage::assign(const Document& document) {
    clear();
    try {
        const std::size_t block_count = std::min(document.blocks.size(), MAX_BLOCKS);
        const std::size_t link_count = std::min(document.links.size(), MAX_LINKS);
        std::size_t run_count = 0;
        std::size_t arena_size = 0;
        for (std::size_t i = 0; i < block_count; ++i) {
            const std::size_t retained = std::min(document.blocks[i].runs.size(),
                MAX_RUNS - std::min(run_count, MAX_RUNS));
            run_count += retained;
            for (std::size_t r = 0; r < retained; ++r) {
                const std::size_t bytes = document.blocks[i].runs[r].text.size() + 1;
                if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
                arena_size += bytes;
            }
        }
        for (std::size_t i = 0; i < link_count; ++i) {
            const std::size_t bytes = document.links[i].target.size() +
                (document.links[i].fields.empty() ? 0 : document.links[i].fields.size() + 1) + 1;
            if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
            arena_size += bytes;
        }
        _arena.reserve(arena_size);
        _blocks.reserve(block_count);
        _runs.reserve(std::min(run_count, MAX_RUNS));
        _links.reserve(link_count);

        for (std::size_t i = 0; i < link_count; ++i) {
            LinkRecord link;
            std::string navigation_target = document.links[i].target;
            if (!document.links[i].fields.empty()) {
                navigation_target += '`';
                navigation_target += document.links[i].fields;
            }
            if (!append(navigation_target, link.target_offset, link.target_length)) {
                clear();
                return false;
            }
            _links.push_back(link);
        }

        for (std::size_t i = 0; i < block_count && _runs.size() < MAX_RUNS; ++i) {
            const auto& source_block = document.blocks[i];
            BlockRecord block;
            block.first_run = static_cast<uint32_t>(_runs.size());
            block.type = source_block.type;
            block.depth = source_block.depth;
            block.alignment = source_block.alignment;
            for (const auto& source_run : source_block.runs) {
                if (_runs.size() >= MAX_RUNS || block.run_count == std::numeric_limits<uint16_t>::max()) {
                    _truncated = true;
                    break;
                }
                RunRecord run;
                if (!append_display(source_run.text, run.text_offset, run.text_length)) {
                    clear();
                    return false;
                }
                run.link_index = source_run.link_index >= 0 &&
                    static_cast<std::size_t>(source_run.link_index) < _links.size()
                        ? static_cast<int16_t>(source_run.link_index) : -1;
                if (source_run.bold) run.style |= BOLD;
                if (source_run.italic) run.style |= ITALIC;
                if (source_run.underline) run.style |= UNDERLINE;
                if (source_run.has_foreground) run.style |= HAS_FOREGROUND;
                if (source_run.has_background) run.style |= HAS_BACKGROUND;
                run.foreground = source_run.foreground;
                run.background = source_run.background;
                _runs.push_back(run);
                ++block.run_count;
            }
            _blocks.push_back(block);
        }

        _has_background = document.has_background;
        _background = document.background;
        _has_foreground = document.has_foreground;
        _foreground = document.foreground;
        _truncated = _truncated || document.truncated || document.blocks.size() > block_count ||
            document.links.size() > link_count;
        _unsupported = document.unsupported;
        return true;
    } catch (const std::bad_alloc&) {
        clear();
        return false;
    }
}

void CompactPage::clear() {
    ExternalVector<char>().swap(_arena);
    ExternalVector<BlockRecord>().swap(_blocks);
    ExternalVector<RunRecord>().swap(_runs);
    ExternalVector<LinkRecord>().swap(_links);
    _has_background = false;
    _background = 0;
    _has_foreground = false;
    _foreground = 0;
    _truncated = false;
    _unsupported = false;
}

bool CompactPage::append_notice(const std::string& value) {
    if (_blocks.size() >= MAX_BLOCKS || _runs.size() >= MAX_RUNS) return false;
    try {
        RunRecord run;
        if (!append(value, run.text_offset, run.text_length)) return false;
        BlockRecord block;
        block.first_run = static_cast<uint32_t>(_runs.size());
        block.run_count = 1;
        block.type = BlockType::UNSUPPORTED;
        _runs.push_back(run);
        _blocks.push_back(block);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

CompactPage::TextView CompactPage::text(const RunRecord& run) const {
    if (run.text_offset > _arena.size() || run.text_length > _arena.size() - run.text_offset) return {};
    return {_arena.data() + run.text_offset, run.text_length};
}

CompactPage::TextView CompactPage::target(std::size_t index) const {
    if (index >= _links.size()) return {};
    const auto& link = _links[index];
    if (link.target_offset > _arena.size() || link.target_length > _arena.size() - link.target_offset) return {};
    return {_arena.data() + link.target_offset, link.target_length};
}

} // namespace UI::LXMF::NomadNet
