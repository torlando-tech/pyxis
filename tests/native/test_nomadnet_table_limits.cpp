#include "NomadNetDocument.h"

#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

using UI::LXMF::NomadNet::DocumentParser;
using UI::LXMF::NomadNet::TruncationReason;

namespace {
bool reject_large_allocations = false;
constexpr std::size_t MAX_ALLOWED_TEMPORARY = 5000;
}

void* operator new(std::size_t size) {
    if (reject_large_allocations && size > MAX_ALLOWED_TEMPORARY) throw std::bad_alloc();
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (reject_large_allocations && size > MAX_ALLOWED_TEMPORARY) throw std::bad_alloc();
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    DocumentParser parser;
    std::string separator_heavy = "`t\n";
    separator_heavy += "h0|h1|h2|h3|h4|h5|h6|h7";
    separator_heavy += std::string(1800, '|');
    separator_heavy += "\n---|---|---|---|---|---|---|---";
    separator_heavy += std::string(1800, '|');
    separator_heavy += "\nv0|v1|v2|v3|v4|v5|v6|v7";
    separator_heavy += std::string(300, '|');
    separator_heavy += "\n`t\nafter";

    bool escaped = false;
    UI::LXMF::NomadNet::Document document;
    reject_large_allocations = true;
    try {
        document = parser.parse(separator_heavy);
    } catch (const std::bad_alloc&) {
        escaped = true;
    }
    reject_large_allocations = false;
    if (escaped || document.tables.size() != 1 ||
        document.tables[0].column_count != DocumentParser::MAX_TABLE_COLUMNS ||
        !document.has_truncation(TruncationReason::TABLE_COLUMNS) ||
        document.table_cells.size() > DocumentParser::MAX_TABLE_CELLS ||
        document.blocks.empty()) {
        std::cerr << "separator-heavy tokenization escaped its bounded allocation contract\n";
        return 1;
    }

    std::string exact = "`t\n";
    for (std::size_t row = 0; row <= DocumentParser::MAX_TABLE_ROWS; ++row) {
        for (std::size_t column = 0; column < DocumentParser::MAX_TABLE_COLUMNS; ++column) {
            if (column) exact += '|';
            exact += row == 1 ? "---" : "x";
        }
        exact += '\n';
    }
    exact += "`t";
    const auto boundary = parser.parse(exact);
    if (boundary.tables.size() != 1 ||
        boundary.tables[0].row_count != DocumentParser::MAX_TABLE_ROWS ||
        boundary.tables[0].column_count != DocumentParser::MAX_TABLE_COLUMNS ||
        boundary.table_cells.size() != DocumentParser::MAX_TABLE_CELLS ||
        boundary.has_truncation(TruncationReason::TABLE_ROWS) ||
        boundary.has_truncation(TruncationReason::TABLE_CELLS)) {
        std::cerr << "exact row/cell boundary changed semantics\n";
        return 1;
    }
    return 0;
}
