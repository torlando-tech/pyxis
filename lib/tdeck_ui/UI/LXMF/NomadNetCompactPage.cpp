#include "NomadNetCompactPage.h"
#include "NomadNetGlyphs.h"

#include <algorithm>
#include <cstring>
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
        const bool reserve_notice = document.truncated || document.unsupported;
        const std::size_t block_limit = MAX_BLOCKS - (reserve_notice ? 1 : 0);
        const std::size_t run_limit = MAX_RUNS - (reserve_notice ? 1 : 0);
        const std::size_t block_count = std::min(document.blocks.size(), block_limit);
        const std::size_t link_count = std::min(document.links.size(), MAX_LINKS);
        const std::size_t field_count = std::min(document.fields.size(), MAX_FIELDS);
        const std::size_t partial_count = std::min(document.partials.size(), MAX_PARTIALS);
        std::size_t partial_field_count = 0;
        for (std::size_t i = 0; i < partial_count; ++i) {
            partial_field_count += std::min(document.partials[i].fields.size(),
                MAX_PARTIAL_FIELDS - std::min(partial_field_count, MAX_PARTIAL_FIELDS));
        }
        const std::size_t table_count = std::min(document.tables.size(), MAX_TABLES);
        std::size_t table_cell_count = 0;
        for (std::size_t i = 0; i < table_count; ++i) {
            const std::size_t cells = static_cast<std::size_t>(document.tables[i].row_count) *
                                      document.tables[i].column_count;
            table_cell_count += std::min(cells,
                MAX_TABLE_CELLS - std::min(table_cell_count, MAX_TABLE_CELLS));
        }
        std::size_t anchor_count = 0;
        for (const auto& anchor : document.anchors) {
            if (anchor_count >= MAX_ANCHORS) break;
            if (anchor.block_index < block_count &&
                anchor.name.size() <= DocumentParser::MAX_ANCHOR_NAME_BYTES)
                ++anchor_count;
        }
        std::size_t run_count = 0;
        std::size_t arena_size = 0;
        for (std::size_t i = 0; i < block_count; ++i) {
            const std::size_t retained = std::min(document.blocks[i].runs.size(),
                run_limit - std::min(run_count, run_limit));
            run_count += retained;
            for (std::size_t r = 0; r < retained; ++r) {
                const std::size_t bytes = document.blocks[i].runs[r].text.size() + 1;
                if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
                arena_size += bytes;
            }
        }
        const std::size_t table_run_count = std::min(document.table_runs.size(),
            run_limit - std::min(run_count, run_limit));
        for (std::size_t r = 0; r < table_run_count; ++r) {
            const std::size_t bytes = document.table_runs[r].text.size() + 1;
            if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
            arena_size += bytes;
        }
        run_count += table_run_count;
        for (std::size_t i = 0; i < link_count; ++i) {
            const std::size_t bytes = document.links[i].target.size() +
                (document.links[i].has_fields ? document.links[i].fields.size() + 1 : 0) + 1;
            if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
            arena_size += bytes;
        }
        for (std::size_t i = 0; i < field_count; ++i) {
            const auto& field = document.fields[i];
            const std::size_t bytes = field.name.size() + field.value.size() + field.label.size() + 3;
            if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
            arena_size += bytes;
        }
        for (std::size_t i = 0; i < partial_count; ++i) {
            const auto& partial = document.partials[i];
            std::size_t bytes = partial.descriptor.size() + partial.url.size() +
                partial.selectors.size() + partial.id.size() + 4;
            for (const auto& field : partial.fields) bytes += field.size() + 1;
            if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
            arena_size += bytes;
        }
        std::size_t anchors_accounted = 0;
        for (const auto& anchor : document.anchors) {
            if (anchors_accounted >= anchor_count) break;
            if (anchor.block_index >= block_count ||
                anchor.name.size() > DocumentParser::MAX_ANCHOR_NAME_BYTES) continue;
            const std::size_t bytes = anchor.name.size() + 1;
            if (bytes > MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES)) return false;
            arena_size += bytes;
            ++anchors_accounted;
        }
        _arena.reserve(arena_size);
        _blocks.reserve(block_count);
        _runs.reserve(std::min(run_count, run_limit));
        _links.reserve(link_count);
        _fields.reserve(field_count);
        _anchors.reserve(anchor_count);
        _tables.reserve(table_count);
        _table_cells.reserve(table_cell_count);
        _partials.reserve(partial_count);
        _partial_fields.reserve(partial_field_count);

        for (std::size_t i = 0; i < link_count; ++i) {
            LinkRecord link;
            std::string navigation_target = document.links[i].target;
            if (document.links[i].has_fields) {
                navigation_target += '`';
                navigation_target += document.links[i].fields;
            }
            if (!append(navigation_target, link.target_offset, link.target_length)) {
                clear();
                return false;
            }
            _links.push_back(link);
        }

        for (std::size_t i = 0; i < field_count; ++i) {
            const auto& source = document.fields[i];
            FieldRecord field;
            if (!append(source.name, field.name_offset, field.name_length) ||
                !append(source.value, field.value_offset, field.value_length) ||
                !append(source.label, field.label_offset, field.label_length)) {
                clear();
                return false;
            }
            field.width = source.width;
            field.type = source.type;
            field.checked = source.checked;
            field.masked = source.masked;
            _fields.push_back(field);
        }

        for (std::size_t i = 0; i < partial_count; ++i) {
            const auto& source = document.partials[i];
            PartialRecord partial;
            partial.first_field = static_cast<uint32_t>(_partial_fields.size());
            if (!append(source.descriptor, partial.descriptor_offset, partial.descriptor_length) ||
                !append(source.url, partial.url_offset, partial.url_length) ||
                !append(source.selectors, partial.selectors_offset, partial.selectors_length) ||
                !append(source.id, partial.id_offset, partial.id_length)) {
                clear();
                return false;
            }
            partial.refresh_interval_ms = source.refresh_interval_ms;
            partial.descriptor_hash = source.descriptor_hash;
            for (const auto& source_field : source.fields) {
                if (_partial_fields.size() >= MAX_PARTIAL_FIELDS ||
                    partial.field_count == std::numeric_limits<uint16_t>::max()) break;
                PartialFieldRecord field;
                if (!append(source_field, field.value_offset, field.value_length)) {
                    clear();
                    return false;
                }
                _partial_fields.push_back(field);
                ++partial.field_count;
            }
            _partials.push_back(partial);
        }

        for (const auto& source_anchor : document.anchors) {
            if (_anchors.size() >= anchor_count) break;
            if (source_anchor.block_index >= block_count ||
                source_anchor.name.size() > DocumentParser::MAX_ANCHOR_NAME_BYTES) continue;
            AnchorRecord anchor;
            if (!append(source_anchor.name, anchor.name_offset, anchor.name_length)) {
                clear();
                return false;
            }
            anchor.block_index = source_anchor.block_index;
            _anchors.push_back(anchor);
        }

        for (std::size_t i = 0; i < block_count; ++i) {
            const auto& source_block = document.blocks[i];
            if (!source_block.runs.empty() && _runs.size() >= run_limit) {
                _truncated = true;
                continue;
            }
            BlockRecord block;
            block.first_run = static_cast<uint32_t>(_runs.size());
            block.type = source_block.type;
            block.depth = source_block.depth;
            block.alignment = source_block.alignment;
            block.divider_codepoint = source_block.divider_codepoint;
            block.table_index = source_block.table_index >= 0 &&
                static_cast<std::size_t>(source_block.table_index) < table_count
                    ? source_block.table_index : -1;
            block.partial_index = source_block.partial_index >= 0 &&
                static_cast<std::size_t>(source_block.partial_index) < partial_count
                    ? source_block.partial_index : -1;
            for (const auto& source_run : source_block.runs) {
                if (_runs.size() >= run_limit || block.run_count == std::numeric_limits<uint16_t>::max()) {
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
                run.field_index = source_run.field_index >= 0 &&
                    static_cast<std::size_t>(source_run.field_index) < _fields.size()
                        ? static_cast<int16_t>(source_run.field_index) : -1;
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

        for (std::size_t i = 0; i < table_count; ++i) {
            const auto& source_table = document.tables[i];
            const std::size_t source_cells = static_cast<std::size_t>(source_table.row_count) *
                                             source_table.column_count;
            if (source_table.first_cell > document.table_cells.size() ||
                source_cells > document.table_cells.size() - source_table.first_cell) {
                clear();
                return false;
            }
            TableRecord table;
            table.first_cell = static_cast<uint32_t>(_table_cells.size());
            table.column_count = source_table.column_count;
            table.alignment = source_table.alignment;
            table.max_width = source_table.max_width;
            const std::size_t cells_left = MAX_TABLE_CELLS - std::min(_table_cells.size(), MAX_TABLE_CELLS);
            const std::size_t retained_cells = std::min(source_cells, cells_left);
            for (std::size_t c = 0; c < retained_cells; ++c) {
                const auto& source_cell = document.table_cells[source_table.first_cell + c];
                if (source_cell.first_run > document.table_runs.size() ||
                    source_cell.run_count > document.table_runs.size() - source_cell.first_run) {
                    clear();
                    return false;
                }
                TableCellRecord cell;
                cell.first_run = static_cast<uint32_t>(_runs.size());
                cell.alignment = source_cell.alignment;
                for (std::size_t r = 0; r < source_cell.run_count; ++r) {
                    if (_runs.size() >= run_limit) {
                        _truncated = true;
                        break;
                    }
                    const auto& source_run = document.table_runs[source_cell.first_run + r];
                    RunRecord run;
                    if (!append_display(source_run.text, run.text_offset, run.text_length)) {
                        clear();
                        return false;
                    }
                    run.link_index = source_run.link_index >= 0 &&
                        static_cast<std::size_t>(source_run.link_index) < _links.size()
                            ? static_cast<int16_t>(source_run.link_index) : -1;
                    run.field_index = source_run.field_index >= 0 &&
                        static_cast<std::size_t>(source_run.field_index) < _fields.size()
                            ? static_cast<int16_t>(source_run.field_index) : -1;
                    if (source_run.bold) run.style |= BOLD;
                    if (source_run.italic) run.style |= ITALIC;
                    if (source_run.underline) run.style |= UNDERLINE;
                    if (source_run.has_foreground) run.style |= HAS_FOREGROUND;
                    if (source_run.has_background) run.style |= HAS_BACKGROUND;
                    run.foreground = source_run.foreground;
                    run.background = source_run.background;
                    _runs.push_back(run);
                    ++cell.run_count;
                }
                _table_cells.push_back(cell);
            }
            table.row_count = table.column_count == 0 ? 0 :
                static_cast<uint16_t>(retained_cells / table.column_count);
            if (retained_cells != source_cells) _truncated = true;
            _tables.push_back(table);
        }

        _has_background = document.has_background;
        _background = document.background;
        _has_foreground = document.has_foreground;
        _foreground = document.foreground;
        _truncated = _truncated || document.truncated || document.blocks.size() > block_count ||
            document.links.size() > link_count || document.anchors.size() > anchor_count ||
            document.fields.size() > field_count || document.tables.size() > table_count ||
            document.partials.size() > partial_count ||
            document.table_cells.size() > table_cell_count ||
            document.table_runs.size() > table_run_count;
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
    ExternalVector<AnchorRecord>().swap(_anchors);
    ExternalVector<TableRecord>().swap(_tables);
    ExternalVector<TableCellRecord>().swap(_table_cells);
    ExternalVector<FieldRecord>().swap(_fields);
    ExternalVector<PartialRecord>().swap(_partials);
    ExternalVector<PartialFieldRecord>().swap(_partial_fields);
    _has_background = false;
    _background = 0;
    _has_foreground = false;
    _foreground = 0;
    _truncated = false;
    _unsupported = false;
}

bool CompactPage::append_notice(const std::string& value) {
    if (value.size() > MAX_NOTICE_BYTES ||
        value.size() + 1 > MAX_ARENA_BYTES - std::min(_arena.size(), MAX_ARENA_BYTES))
        return false;
    if (_blocks.size() >= MAX_BLOCKS) {
        const BlockRecord removed = _blocks.back();
        const std::size_t first = std::min<std::size_t>(removed.first_run, _runs.size());
        _runs.resize(first);
        _blocks.pop_back();
    }
    if (_runs.size() >= MAX_RUNS) {
        std::size_t owner = _blocks.size();
        while (owner > 0 && _blocks[owner - 1].run_count == 0) --owner;
        if (owner == 0) return false;
        _runs.pop_back();
        --_blocks[owner - 1].run_count;
        for (std::size_t i = owner; i < _blocks.size(); ++i)
            _blocks[i].first_run = static_cast<uint32_t>(_runs.size());
    }
    _anchors.erase(std::remove_if(_anchors.begin(),_anchors.end(),
        [this](const AnchorRecord& anchor){return anchor.block_index>=_blocks.size();}),
        _anchors.end());
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
        clear();
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

CompactPage::TextView CompactPage::field_name(std::size_t index) const {
    if (index >= _fields.size()) return {};
    const auto& field = _fields[index];
    if (field.name_offset > _arena.size() || field.name_length > _arena.size() - field.name_offset) return {};
    return {_arena.data() + field.name_offset, field.name_length};
}

CompactPage::TextView CompactPage::field_value(std::size_t index) const {
    if (index >= _fields.size()) return {};
    const auto& field = _fields[index];
    if (field.value_offset > _arena.size() || field.value_length > _arena.size() - field.value_offset) return {};
    return {_arena.data() + field.value_offset, field.value_length};
}

CompactPage::TextView CompactPage::field_label(std::size_t index) const {
    if (index >= _fields.size()) return {};
    const auto& field = _fields[index];
    if (field.label_offset > _arena.size() || field.label_length > _arena.size() - field.label_offset) return {};
    return {_arena.data() + field.label_offset, field.label_length};
}

CompactPage::TextView CompactPage::partial_descriptor(const PartialRecord& partial) const {
    if (partial.descriptor_offset > _arena.size() ||
        partial.descriptor_length > _arena.size() - partial.descriptor_offset) return {};
    return {_arena.data() + partial.descriptor_offset, partial.descriptor_length};
}

CompactPage::TextView CompactPage::partial_url(const PartialRecord& partial) const {
    if (partial.url_offset > _arena.size() ||
        partial.url_length > _arena.size() - partial.url_offset) return {};
    return {_arena.data() + partial.url_offset, partial.url_length};
}

CompactPage::TextView CompactPage::partial_selectors(const PartialRecord& partial) const {
    if (partial.selectors_offset > _arena.size() ||
        partial.selectors_length > _arena.size() - partial.selectors_offset) return {};
    return {_arena.data() + partial.selectors_offset, partial.selectors_length};
}

CompactPage::TextView CompactPage::partial_id(const PartialRecord& partial) const {
    if (partial.id_offset > _arena.size() ||
        partial.id_length > _arena.size() - partial.id_offset) return {};
    return {_arena.data() + partial.id_offset, partial.id_length};
}

CompactPage::TextView CompactPage::partial_field(const PartialRecord& partial,
                                                  std::size_t index) const {
    if (index >= partial.field_count || partial.first_field > _partial_fields.size() ||
        index >= _partial_fields.size() - partial.first_field) return {};
    const auto& field = _partial_fields[partial.first_field + index];
    if (field.value_offset > _arena.size() ||
        field.value_length > _arena.size() - field.value_offset) return {};
    return {_arena.data() + field.value_offset, field.value_length};
}

bool CompactPage::find_anchor(const std::string& name, uint16_t& block_index) const {
    if (name.size() > DocumentParser::MAX_ANCHOR_NAME_BYTES) return false;
    for (const auto& anchor : _anchors) {
        if (anchor.name_offset > _arena.size() ||
            anchor.name_length > _arena.size() - anchor.name_offset) continue;
        if (anchor.name_length == name.size() &&
            std::memcmp(_arena.data() + anchor.name_offset, name.data(), name.size()) == 0) {
            block_index = anchor.block_index;
            return true;
        }
    }
    return false;
}

} // namespace UI::LXMF::NomadNet
