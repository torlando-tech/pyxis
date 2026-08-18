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
        const std::size_t notice_slack = std::min(
            MAX_NOTICE_BYTES + 1,
            MAX_ARENA_BYTES - std::min(arena_size, MAX_ARENA_BYTES));
        _arena.reserve(arena_size + notice_slack);
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
            block.partial_region_index = source_block.partial_region_index >= 0 &&
                static_cast<std::size_t>(source_block.partial_region_index) < partial_count
                    ? source_block.partial_region_index : -1;
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
                if (run.link_index >= 0)
                    _links[run.link_index].partial_region_index =
                        block.partial_region_index;
                if (run.field_index >= 0)
                    _fields[run.field_index].partial_region_index =
                        block.partial_region_index;
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

PartialReplaceResult CompactPage::assign_replacing_partial(
        const CompactPage& base, std::size_t partial_index,
        const Document& fragment, std::size_t max_arena_bytes) {
    clear();
    if (partial_index >= base._partials.size())
        return PartialReplaceResult::INVALID_PARTIAL;
    if (fragment.allocation_failed)
        return PartialReplaceResult::ALLOCATION_FAILED;
    if (fragment.malformed || fragment.truncated ||
            max_arena_bytes > MAX_ARENA_BYTES)
        return PartialReplaceResult::LIMIT_EXCEEDED;

    bool region_found = false;
    for (const auto& block : base._blocks)
        if (block.partial_region_index == static_cast<int16_t>(partial_index)) {
            region_found = true;
            break;
        }
    if (!region_found) return PartialReplaceResult::INVALID_PARTIAL;

    CompactPage fragment_page;
    if (!fragment_page.assign(fragment))
        return PartialReplaceResult::ALLOCATION_FAILED;
    if (fragment_page.truncated())
        return PartialReplaceResult::LIMIT_EXCEEDED;

    bool limit_failed = false;
    try {
        const std::size_t source_arena = std::min(
            max_arena_bytes, base._arena.size() + fragment_page._arena.size());
        const std::size_t reserve_arena = source_arena + std::min(
            MAX_NOTICE_BYTES + 1, max_arena_bytes - source_arena);
        _arena.reserve(reserve_arena);
        _blocks.reserve(std::min(MAX_BLOCKS,
            base._blocks.size() + fragment_page._blocks.size()));
        _runs.reserve(std::min(MAX_RUNS,
            base._runs.size() + fragment_page._runs.size()));
        _links.reserve(std::min(MAX_LINKS,
            base._links.size() + fragment_page._links.size()));
        _fields.reserve(std::min(MAX_FIELDS,
            base._fields.size() + fragment_page._fields.size()));
        _anchors.reserve(std::min(MAX_ANCHORS,
            base._anchors.size() + fragment_page._anchors.size()));
        _tables.reserve(std::min(MAX_TABLES,
            base._tables.size() + fragment_page._tables.size()));
        _table_cells.reserve(std::min(MAX_TABLE_CELLS,
            base._table_cells.size() + fragment_page._table_cells.size()));
        _partials.reserve(base._partials.size());
        _partial_fields.reserve(base._partial_fields.size());

        auto append_view = [&](TextView value, uint32_t& offset,
                               uint16_t& length) -> bool {
            if ((!value.data() && !value.empty()) ||
                    value.size() > std::numeric_limits<uint16_t>::max() ||
                    value.size() + 1 > max_arena_bytes -
                        std::min(_arena.size(), max_arena_bytes)) {
                limit_failed = true;
                return false;
            }
            offset = static_cast<uint32_t>(_arena.size());
            length = static_cast<uint16_t>(value.size());
            if (!value.empty())
                _arena.insert(_arena.end(), value.data(), value.data() + value.size());
            _arena.push_back('\0');
            return true;
        };

        for (const auto& source : base._partials) {
            PartialRecord partial;
            partial.first_field = static_cast<uint32_t>(_partial_fields.size());
            if (!append_view(base.partial_descriptor(source), partial.descriptor_offset,
                             partial.descriptor_length) ||
                    !append_view(base.partial_url(source), partial.url_offset,
                                 partial.url_length) ||
                    !append_view(base.partial_selectors(source), partial.selectors_offset,
                                 partial.selectors_length) ||
                    !append_view(base.partial_id(source), partial.id_offset,
                                 partial.id_length)) {
                clear();
                return PartialReplaceResult::LIMIT_EXCEEDED;
            }
            partial.refresh_interval_ms = source.refresh_interval_ms;
            partial.descriptor_hash = source.descriptor_hash;
            for (std::size_t field_index = 0;
                    field_index < source.field_count; ++field_index) {
                if (_partial_fields.size() >= MAX_PARTIAL_FIELDS) {
                    clear();
                    return PartialReplaceResult::LIMIT_EXCEEDED;
                }
                PartialFieldRecord field;
                if (!append_view(base.partial_field(source, field_index),
                                 field.value_offset, field.value_length)) {
                    clear();
                    return PartialReplaceResult::LIMIT_EXCEEDED;
                }
                _partial_fields.push_back(field);
                ++partial.field_count;
            }
            _partials.push_back(partial);
        }

        struct CopyMaps {
            ExternalVector<int16_t> links;
            ExternalVector<int16_t> fields;
            ExternalVector<int16_t> tables;
            ExternalVector<int32_t> blocks;
        };
        auto make_maps = [](const CompactPage& source) {
            CopyMaps maps;
            maps.links.assign(source._links.size(), -1);
            maps.fields.assign(source._fields.size(), -1);
            maps.tables.assign(source._tables.size(), -1);
            maps.blocks.assign(source._blocks.size(), -1);
            return maps;
        };
        CopyMaps base_maps = make_maps(base);
        CopyMaps fragment_maps = make_maps(fragment_page);

        auto copy_link = [&](const CompactPage& source, CopyMaps& maps,
                             int16_t source_index) -> int16_t {
            if (source_index < 0 ||
                    static_cast<std::size_t>(source_index) >= source._links.size())
                return -1;
            int16_t& mapped = maps.links[source_index];
            if (mapped >= 0) return mapped;
            if (_links.size() >= MAX_LINKS) {
                limit_failed = true;
                return -1;
            }
            LinkRecord link;
            if (!append_view(source.target(source_index), link.target_offset,
                             link.target_length))
                return -1;
            link.partial_region_index =
                source._links[source_index].partial_region_index;
            mapped = static_cast<int16_t>(_links.size());
            _links.push_back(link);
            return mapped;
        };
        auto copy_field = [&](const CompactPage& source, CopyMaps& maps,
                              int16_t source_index) -> int16_t {
            if (source_index < 0 ||
                    static_cast<std::size_t>(source_index) >= source._fields.size())
                return -1;
            int16_t& mapped = maps.fields[source_index];
            if (mapped >= 0) return mapped;
            if (_fields.size() >= MAX_FIELDS) {
                limit_failed = true;
                return -1;
            }
            const auto& old = source._fields[source_index];
            FieldRecord field;
            if (!append_view(source.field_name(source_index), field.name_offset,
                             field.name_length) ||
                    !append_view(source.field_value(source_index), field.value_offset,
                                 field.value_length) ||
                    !append_view(source.field_label(source_index), field.label_offset,
                                 field.label_length))
                return -1;
            field.width = old.width;
            field.type = old.type;
            field.checked = old.checked;
            field.masked = old.masked;
            field.partial_region_index = old.partial_region_index;
            mapped = static_cast<int16_t>(_fields.size());
            _fields.push_back(field);
            return mapped;
        };
        auto copy_run = [&](const CompactPage& source, CopyMaps& maps,
                            const RunRecord& old, RunRecord& run) -> bool {
            if (_runs.size() >= MAX_RUNS) {
                limit_failed = true;
                return false;
            }
            if (!append_view(source.text(old), run.text_offset, run.text_length))
                return false;
            run.link_index = copy_link(source, maps, old.link_index);
            run.field_index = copy_field(source, maps, old.field_index);
            if (&source == &fragment_page) {
                if (run.link_index >= 0)
                    _links[run.link_index].partial_region_index =
                        static_cast<int16_t>(partial_index);
                if (run.field_index >= 0)
                    _fields[run.field_index].partial_region_index =
                        static_cast<int16_t>(partial_index);
            }
            run.style = old.style;
            run.foreground = old.foreground;
            run.background = old.background;
            if ((old.link_index >= 0 && run.link_index < 0) ||
                    (old.field_index >= 0 && run.field_index < 0))
                return false;
            return true;
        };
        auto copy_table = [&](const CompactPage& source, CopyMaps& maps,
                              int16_t source_index) -> int16_t {
            if (source_index < 0 ||
                    static_cast<std::size_t>(source_index) >= source._tables.size())
                return -1;
            int16_t& mapped = maps.tables[source_index];
            if (mapped >= 0) return mapped;
            if (_tables.size() >= MAX_TABLES) {
                limit_failed = true;
                return -1;
            }
            const auto& old_table = source._tables[source_index];
            const std::size_t cell_count = static_cast<std::size_t>(old_table.row_count) *
                                           old_table.column_count;
            if (old_table.first_cell > source._table_cells.size() ||
                    cell_count > source._table_cells.size() - old_table.first_cell ||
                    cell_count > MAX_TABLE_CELLS -
                        std::min(_table_cells.size(), MAX_TABLE_CELLS)) {
                limit_failed = true;
                return -1;
            }
            TableRecord table;
            table.first_cell = static_cast<uint32_t>(_table_cells.size());
            table.row_count = old_table.row_count;
            table.column_count = old_table.column_count;
            table.alignment = old_table.alignment;
            table.max_width = old_table.max_width;
            for (std::size_t cell_index = 0; cell_index < cell_count; ++cell_index) {
                const auto& old_cell = source._table_cells[old_table.first_cell + cell_index];
                if (old_cell.first_run > source._runs.size() ||
                        old_cell.run_count > source._runs.size() - old_cell.first_run) {
                    limit_failed = true;
                    return -1;
                }
                TableCellRecord cell;
                cell.first_run = static_cast<uint32_t>(_runs.size());
                cell.alignment = old_cell.alignment;
                for (std::size_t run_index = 0; run_index < old_cell.run_count; ++run_index) {
                    RunRecord run;
                    if (!copy_run(source, maps,
                                  source._runs[old_cell.first_run + run_index], run))
                        return -1;
                    _runs.push_back(run);
                    ++cell.run_count;
                }
                _table_cells.push_back(cell);
            }
            mapped = static_cast<int16_t>(_tables.size());
            _tables.push_back(table);
            return mapped;
        };

        auto copy_block = [&](const CompactPage& source, CopyMaps& maps,
                              std::size_t source_index, int16_t region_override,
                              bool retain_partial_index) -> bool {
            if (source_index >= source._blocks.size() || _blocks.size() >= MAX_BLOCKS) {
                limit_failed = true;
                return false;
            }
            const auto& old = source._blocks[source_index];
            if (old.first_run > source._runs.size() ||
                    old.run_count > source._runs.size() - old.first_run) {
                limit_failed = true;
                return false;
            }
            BlockRecord block;
            block.first_run = static_cast<uint32_t>(_runs.size());
            block.type = old.type;
            block.depth = old.depth;
            block.alignment = old.alignment;
            block.divider_codepoint = old.divider_codepoint;
            block.partial_index = retain_partial_index ? old.partial_index : -1;
            block.partial_region_index = region_override >= 0
                ? region_override : old.partial_region_index;
            if (old.table_index >= 0) {
                block.table_index = copy_table(source, maps, old.table_index);
                if (block.table_index < 0) return false;
            }
            for (std::size_t run_index = 0; run_index < old.run_count; ++run_index) {
                RunRecord run;
                if (!copy_run(source, maps,
                              source._runs[old.first_run + run_index], run))
                    return false;
                _runs.push_back(run);
                ++block.run_count;
            }
            maps.blocks[source_index] = static_cast<int32_t>(_blocks.size());
            _blocks.push_back(block);
            return true;
        };

        bool inserted = false;
        for (std::size_t block_index = 0; block_index < base._blocks.size(); ++block_index) {
            const auto& block = base._blocks[block_index];
            if (block.partial_region_index == static_cast<int16_t>(partial_index)) {
                if (inserted) continue;
                inserted = true;
                if (fragment_page._blocks.empty()) {
                    if (_blocks.size() >= MAX_BLOCKS) {
                        limit_failed = true;
                        break;
                    }
                    BlockRecord marker;
                    marker.first_run = static_cast<uint32_t>(_runs.size());
                    marker.partial_region_index = static_cast<int16_t>(partial_index);
                    _blocks.push_back(marker);
                } else {
                    for (std::size_t fragment_index = 0;
                            fragment_index < fragment_page._blocks.size(); ++fragment_index)
                        if (!copy_block(fragment_page, fragment_maps, fragment_index,
                                        static_cast<int16_t>(partial_index), false)) {
                            limit_failed = true;
                            break;
                        }
                }
                if (limit_failed) break;
                continue;
            }
            if (!copy_block(base, base_maps, block_index, -1, true)) {
                limit_failed = true;
                break;
            }
        }
        if (!inserted || limit_failed) {
            clear();
            return PartialReplaceResult::LIMIT_EXCEEDED;
        }

        auto copy_anchors = [&](const CompactPage& source, const CopyMaps& maps) -> bool {
            for (const auto& old : source._anchors) {
                if (old.block_index >= maps.blocks.size()) continue;
                const int32_t mapped_block = maps.blocks[old.block_index];
                if (mapped_block < 0) continue;
                if (_anchors.size() >= MAX_ANCHORS) {
                    limit_failed = true;
                    return false;
                }
                AnchorRecord anchor;
                const TextView name(source._arena.data() + old.name_offset,
                                    old.name_length);
                if (!append_view(name, anchor.name_offset, anchor.name_length))
                    return false;
                anchor.block_index = static_cast<uint16_t>(mapped_block);
                _anchors.push_back(anchor);
            }
            return true;
        };
        if (!copy_anchors(base, base_maps) ||
                !copy_anchors(fragment_page, fragment_maps) || limit_failed) {
            clear();
            return PartialReplaceResult::LIMIT_EXCEEDED;
        }
        std::stable_sort(_anchors.begin(), _anchors.end(),
            [](const AnchorRecord& left, const AnchorRecord& right) {
                return left.block_index < right.block_index;
            });

        _has_background = base._has_background;
        _background = base._background;
        _has_foreground = base._has_foreground;
        _foreground = base._foreground;
        _truncated = base._truncated;
        _unsupported = base._unsupported || fragment_page._unsupported;
        return PartialReplaceResult::APPLIED;
    } catch (const std::bad_alloc&) {
        clear();
        return PartialReplaceResult::ALLOCATION_FAILED;
    }
}

void CompactPage::clear() {
    if (!_arena.empty()) {
        volatile char* bytes = _arena.data();
        for (std::size_t index = 0; index < _arena.size(); ++index)
            bytes[index] = 0;
    }
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

CompactPage& CompactPage::operator=(CompactPage&& other) noexcept {
    if (this == &other) return *this;
    clear();
    _arena.swap(other._arena);
    _blocks.swap(other._blocks);
    _runs.swap(other._runs);
    _links.swap(other._links);
    _anchors.swap(other._anchors);
    _tables.swap(other._tables);
    _table_cells.swap(other._table_cells);
    _fields.swap(other._fields);
    _partials.swap(other._partials);
    _partial_fields.swap(other._partial_fields);
    std::swap(_has_background, other._has_background);
    std::swap(_background, other._background);
    std::swap(_has_foreground, other._has_foreground);
    std::swap(_foreground, other._foreground);
    std::swap(_truncated, other._truncated);
    std::swap(_unsupported, other._unsupported);
    return *this;
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
