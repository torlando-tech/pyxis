#define private public
#include "NomadNetCompactPage.h"
#undef private
#include "NomadNetDocument.h"
#include "NomadNetForm.h"
#include "NomadNetLibrary.h"
#include "NomadNetPageApplication.h"
#include "NomadNetOwner.h"
#include "NomadNetUrl.h"

#include <cstdlib>

#include <iostream>
#include <new>
#include <string>

namespace {
std::size_t allocation_index = 0;
std::size_t fail_at = static_cast<std::size_t>(-1);
bool injection_enabled = false;
}

void* operator new(std::size_t size) {
    if (injection_enabled && allocation_index++ == fail_at) throw std::bad_alloc();
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (injection_enabled && allocation_index++ == fail_at) throw std::bad_alloc();
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (injection_enabled && allocation_index++ == fail_at) return nullptr;
    return std::malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (injection_enabled && allocation_index++ == fail_at) return nullptr;
    return std::malloc(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

using namespace UI::LXMF::NomadNet;

static std::size_t retained_capacity(const CompactPage& page) {
    return page._arena.capacity() * sizeof(char) +
        page._blocks.capacity() * sizeof(CompactPage::BlockRecord) +
        page._runs.capacity() * sizeof(CompactPage::RunRecord) +
        page._links.capacity() * sizeof(CompactPage::LinkRecord) +
        page._anchors.capacity() * sizeof(CompactPage::AnchorRecord) +
        page._tables.capacity() * sizeof(CompactPage::TableRecord) +
        page._table_cells.capacity() * sizeof(CompactPage::TableCellRecord) +
        page._fields.capacity() * sizeof(CompactPage::FieldRecord);
}

int main() {
    std::string source = "`t\n";
    for (int row = 0; row < 20; ++row) {
        source += "`!linked`[go`:/page/next.mu]|";
        source += std::string(180, static_cast<char>('a' + row % 20));
        source += "|`<name`value>|tail\n";
        if (row == 1) source += "---|---|---|---\n";
    }
    source += "`t\nafter";

    DocumentParser parser;
    Document baseline;
    if (parser.parse_into(source.data(), source.size(), baseline) != ParseStatus::OK ||
        baseline.tables.empty()) {
        std::cerr << "baseline parse failed\n";
        return 1;
    }

    bool saw_parse_failure = false;
    bool saw_parse_success = false;
    for (std::size_t point = 0; point < 3000; ++point) {
        Document output = parser.parse("old page");
        allocation_index = 0;
        fail_at = point;
        injection_enabled = true;
        const ParseStatus status = parser.parse_into(source.data(), source.size(), output);
        injection_enabled = false;
        if (status == ParseStatus::ALLOCATION_FAILED) {
            saw_parse_failure = true;
            if (!output.blocks.empty() || !output.tables.empty() ||
                output.blocks.capacity() != 0 || output.table_cells.capacity() != 0 ||
                !output.allocation_failed) {
                std::cerr << "parse failure retained partial capacity/model\n";
                return 1;
            }
        } else if (status == ParseStatus::OK) {
            saw_parse_success = true;
            break;
        } else {
            std::cerr << "unexpected parse status\n";
            return 1;
        }
    }
    if (!saw_parse_failure || !saw_parse_success) {
        std::cerr << "parse allocation sweep did not cover failure and recovery\n";
        return 1;
    }

    bool saw_compact_failure = false;
    bool saw_compact_success = false;
    for (std::size_t point = 0; point < 2000; ++point) {
        CompactPage page;
        if (!page.assign(parser.parse("old page"))) return 1;
        allocation_index = 0;
        fail_at = point;
        injection_enabled = true;
        const bool assigned = page.assign(baseline);
        injection_enabled = false;
        if (!assigned) {
            saw_compact_failure = true;
            if (!page.empty() || page.arena_bytes() != 0 ||
                retained_capacity(page) != 0) {
                std::cerr << "compact failure retained partial capacity/model\n";
                return 1;
            }
        } else {
            saw_compact_success = true;
            break;
        }
    }
    if (!saw_compact_failure || !saw_compact_success) {
        std::cerr << "compact allocation sweep did not cover failure and recovery\n";
        return 1;
    }

    bool saw_application_failure = false;
    bool saw_application_success = false;
    for (std::size_t point = 0; point < 3000; ++point) {
        Library library;
        const std::string old_url = "00000000000000000000000000000000:/page/old.mu";
        const std::string new_url = "11111111111111111111111111111111:/page/new.mu#anchor";
        Url current_url;
        std::string url_error;
        if (!Url::parse(new_url, current_url, url_error)) return 1;
        if (!library.record_page(old_url, "Old", 1)) {
            std::cerr << "application baseline library failed\n"; return 1;
        }
        CompactPage visible;
        FormState visible_form;
        if (!visible.assign(parser.parse("old visible")) || !visible_form.assign(visible)) {
            std::cerr << "application baseline visible failed\n"; return 1;
        }
        int history_commits = 0;
        allocation_index = 0;
        fail_at = point;
        injection_enabled = true;
        const auto result = apply_page_transaction_for_url(
            baseline, current_url, 2,
            library,
            [&](const PagePublication&) {
                CompactPage page;
                FormState form;
                std::vector<int> layout;
                if (!page.assign(baseline) || !form.assign(page)) return false;
                layout.assign(page.blocks().size() + page.runs().size() +
                              page.fields().size() + page.tables().size(), 1);
                visible = std::move(page);
                visible_form = std::move(form);
                return true;
            },
            [&]() noexcept { ++history_commits; });
        injection_enabled = false;
        if (result != PageApplyResult::APPLIED) {
            saw_application_failure = true;
            if (history_commits != 0 || library.pages().size() != 1 ||
                library.pages()[0].url != old_url || visible.blocks().size() != 1) {
                std::cerr << "production application failure mutated published owner state\n";
                return 1;
            }
        } else {
            saw_application_success = true;
            if (history_commits != 1 || library.pages().size() != 2 || visible.empty()) {
                std::cerr << "production application success did not commit once\n"; return 1;
            }
            break;
        }
    }
    if (!saw_application_failure || !saw_application_success) {
        std::cerr << "production application allocation sweep incomplete\n";
        return 1;
    }

    class UnusedSubmission final : public OwnerSubmissionSource {
    public:
        bool prepare_submission(uint16_t, uint32_t, std::string&,
                ExternalVector<uint8_t>&, FormEncodeResult&) override { return false; }
    } unused;
    bool saw_owner_failure = false;
    bool saw_owner_success = false;
    const uint8_t retained[] = {0x81, 0xa1, 'x', 0xa1, 'y'};
    for (std::size_t point = 0; point < 100; ++point) {
        PageHistory history;
        if (!history.open("old", true, 0, retained, sizeof(retained)) ||
            !history.open("current", true, 37)) return 1;
        OwnerController owner;
        UserAction back;
        back.kind = UserActionKind::BACK;
        allocation_index = 0;
        fail_at = point;
        injection_enabled = true;
        auto command = owner.service(back, history, unused, 0);
        injection_enabled = false;
        if (command.result == OwnerResult::ALLOCATION_FAILED) {
            saw_owner_failure = true;
            if (history.current() != "current" || history.depth() != 1 ||
                command.pending_history.ready() || !command.request_data.empty()) {
                std::cerr << "owner Back allocation failure mutated history\n";
                return 1;
            }
        } else if (command.result == OwnerResult::REQUEST) {
            saw_owner_success = true;
            if (history.current() != "current" || history.depth() != 1 ||
                command.request_data.size() != sizeof(retained) ||
                !command.pending_history.ready()) return 1;
            break;
        } else return 1;
    }
    if (!saw_owner_failure || !saw_owner_success) {
        std::cerr << "owner Back allocation sweep incomplete\n";
        return 1;
    }
    return 0;
}
