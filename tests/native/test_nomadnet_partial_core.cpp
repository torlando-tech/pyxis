#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

#include "NomadNetCompactPage.h"
#include "NomadNetDocument.h"
#include "NomadNetPartialScheduler.h"

using UI::LXMF::NomadNet::BlockType;
using UI::LXMF::NomadNet::CompactPage;
using UI::LXMF::NomadNet::DocumentParser;
using UI::LXMF::NomadNet::PartialRequest;
using UI::LXMF::NomadNet::PartialScheduler;
using UI::LXMF::NomadNet::TruncationReason;

int main() {
    int failures = 0;
    auto check = [&](const char* name, bool condition) {
        if (!condition) {
            std::cerr << "FAIL: " << name << '\n';
            ++failures;
        }
    };
    auto hex = [](const std::array<uint8_t, 32>& value) {
        std::ostringstream output;
        for (uint8_t byte : value)
            output << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(byte);
        return output.str();
    };

    DocumentParser parser;
    const auto document = parser.parse(
        "before\n"
        "`{f64a846313b874ee4a357040807f8c77:/page/hello.mu`10`pid=32|user_name}\n"
        "after");

    check("canonical partial descriptor is retained",
          document.partials.size() == 1 && document.blocks.size() == 3 &&
          document.blocks[1].type == BlockType::PARTIAL &&
          document.blocks[1].partial_index == 0);
    if (document.partials.size() == 1) {
        const auto& partial = document.partials[0];
        check("partial URL is exact",
              partial.url == "f64a846313b874ee4a357040807f8c77:/page/hello.mu");
        check("partial refresh is represented in milliseconds",
              partial.refresh_interval_ms == 10000U);
        check("partial ID and fields follow canonical components",
              partial.id == "32" && partial.fields.size() == 2 &&
              partial.fields[0] == "pid=32" && partial.fields[1] == "user_name");
        check("partial selectors and canonical SHA-256 identity are retained",
              partial.descriptor ==
                  "f64a846313b874ee4a357040807f8c77:/page/hello.mu`10`pid=32|user_name" &&
              partial.selectors == "pid=32|user_name" &&
              hex(partial.descriptor_hash) ==
                  "412d0e39b4703ab0f4b29f77158f252a66f560207ea5ea83882e270b8e68ea07");
    }

    const auto fractional = parser.parse(
        "`{:relative.mu`.999}\n`{:timed.mu`1.25}");
    check("sub-second canonical refresh disables automatic scheduling",
          fractional.partials.size() == 2 &&
          fractional.partials[0].refresh_interval_ms == 0U);
    check("fractional canonical refresh retains millisecond precision",
          fractional.partials.size() == 2 &&
          fractional.partials[1].refresh_interval_ms == 1250U);
    check("absent selector component retains canonical empty selector",
          fractional.partials[0].fields.size() == 1 &&
          fractional.partials[0].fields[0].empty() &&
          fractional.partials[1].fields.size() == 1 &&
          fractional.partials[1].fields[0].empty());

    const auto escaped = parser.parse("\\`{:escaped.mu}");
    check("escaped canonical partial is still recognized",
          escaped.partials.size() == 1 && escaped.partials[0].url == ":escaped.mu");
    const auto escaped_controls = parser.parse("\\-\n\\>heading\n\\`=");
    check("escaped structural controls remain ordinary literal text",
          escaped_controls.blocks.size() == 3 &&
          escaped_controls.blocks[0].type == BlockType::TEXT &&
          escaped_controls.blocks[0].runs[0].text == "-" &&
          escaped_controls.blocks[1].type == BlockType::TEXT &&
          escaped_controls.blocks[1].runs[0].text == ">heading" &&
          escaped_controls.blocks[2].type == BlockType::TEXT &&
          escaped_controls.blocks[2].runs[0].text == "`=");

    const auto sanitized_comment = parser.parse("># hidden `<name`value>");
    check("heading field sanitization precedes comment classification",
          sanitized_comment.blocks.empty() && sanitized_comment.fields.empty());
    const auto sanitized_table = parser.parse(
        ">`t`<name`value>\nA|B\nC|D\n`t");
    check("heading field sanitization precedes table command recognition",
          sanitized_table.tables.size() == 1 && sanitized_table.fields.empty());
    const auto sanitized_reset = parser.parse(
        ">Heading\n><body `<name`value>");
    check("heading field sanitization precedes section reset",
          sanitized_reset.blocks.size() == 2 &&
          sanitized_reset.blocks[1].depth == 0);
    const auto sanitized_table_row = parser.parse(
        "`t\n>Left|Right `<name`value>\nBottom|Row\n`t");
    check("heading field sanitization occurs before table buffering",
          !sanitized_table_row.table_runs.empty() &&
          sanitized_table_row.table_runs[0].text == "Left");
    const auto reset_comment = parser.parse("<# hidden");
    const auto reset_table = parser.parse("<`t\nA|B\nC|D\n`t");
    const auto reset_heading = parser.parse("<>Heading");
    const auto reset_divider = parser.parse("<-");
    check("section reset restarts canonical line classification",
          reset_comment.blocks.empty() &&
          reset_table.tables.size() == 1 &&
          reset_heading.blocks.size() == 1 &&
          reset_heading.blocks[0].type == BlockType::HEADING &&
          reset_divider.blocks.size() == 1 &&
          reset_divider.blocks[0].type == BlockType::DIVIDER);
    const auto reset_literal = parser.parse("<`=\n-\n`=");
    check("section reset restarts literal-toggle classification",
          reset_literal.blocks.size() == 1 &&
          reset_literal.blocks[0].type == BlockType::TEXT &&
          reset_literal.blocks[0].runs[0].text == "-" &&
          !reset_literal.malformed);

    const auto decimal_refresh = parser.parse(
        "`{:space.mu`1.0 }\n`{:underscore.mu`1_0}\n`{:exponent.mu`1.e2}");
    check("canonical Python decimal refresh grammar is retained",
          decimal_refresh.partials.size() == 3 &&
          decimal_refresh.partials[0].refresh_interval_ms == 1000U &&
          decimal_refresh.partials[1].refresh_interval_ms == 10000U &&
          decimal_refresh.partials[2].refresh_interval_ms == 100000U);
    const auto unicode_refresh = parser.parse(
        u8"`{:unicode.mu`\u00A0\u0661.\u0662\u00A0}");
    check("canonical Unicode decimal digits and outer whitespace are retained",
          unicode_refresh.partials.size() == 1 &&
          unicode_refresh.partials[0].refresh_interval_ms == 1200U);
    bool rejected_information_separators = true;
    for (char control = 0x1c; control <= 0x1f; ++control) {
        const std::string source = "`{:control.mu`" + std::string(1, control) +
            "1.2" + std::string(1, control) + "}";
        const auto control_refresh = parser.parse(source);
        rejected_information_separators = rejected_information_separators &&
            control_refresh.partials.empty() && control_refresh.malformed;
    }
    check("Python-float-rejected information separators remain invalid",
          rejected_information_separators);
    const auto hexadecimal_refresh = parser.parse("`{:hex.mu`0x1p1}");
    check("non-canonical C hexadecimal refresh is rejected",
          hexadecimal_refresh.partials.empty() && hexadecimal_refresh.malformed);

    const auto canonical_vector = parser.parse("`{:/page/a.mu}TRAIL");
    check("first closing brace terminates the descriptor and canonical hash input",
          canonical_vector.partials.size() == 1 &&
          canonical_vector.partials[0].descriptor == ":/page/a.mu" &&
          hex(canonical_vector.partials[0].descriptor_hash) ==
              "07216f28dfbd82d34e655ded06936a911a4ea2cc138b46c0dd5ccb12231f541c");
    const auto refresh_cap = parser.parse(
        "`{:max.mu`604800}\n`{:too-long.mu`604800.001}\n`{:nan.mu`nan}");
    check("refresh interval is finite and bounded below INT32 wrap ambiguity",
          refresh_cap.partials.size() == 1 &&
          refresh_cap.partials[0].refresh_interval_ms ==
              DocumentParser::MAX_PARTIAL_REFRESH_MS && refresh_cap.malformed);

    std::string too_many;
    for (std::size_t index = 0; index <= DocumentParser::MAX_PARTIALS; ++index)
        too_many += "`{:p" + std::to_string(index) + ".mu}\n";
    const auto capped = parser.parse(too_many);
    check("peer-controlled partial count is capped",
          capped.partials.size() == DocumentParser::MAX_PARTIALS &&
          capped.has_truncation(TruncationReason::PARTIALS));

    const auto malformed = parser.parse("`{:broken.mu`not-a-number}");
    check("malformed refresh does not create a schedulable descriptor",
          malformed.partials.empty() && malformed.malformed &&
          malformed.blocks.size() == 1 &&
          malformed.blocks[0].type == BlockType::UNSUPPORTED);

    const auto oversized_url = parser.parse(
        "`{" + std::string(DocumentParser::MAX_PARTIAL_URL_BYTES + 1, 'u') + "}");
    check("partial URL bytes are capped",
          oversized_url.partials.empty() &&
          oversized_url.has_truncation(TruncationReason::PARTIAL_DESCRIPTOR_BYTES));

    std::string excessive_fields = "`{:fields.mu`1`";
    for (std::size_t index = 0; index <= DocumentParser::MAX_PARTIAL_FIELDS; ++index) {
        if (index != 0) excessive_fields += '|';
        excessive_fields += "f" + std::to_string(index);
    }
    excessive_fields += '}';
    const auto fields_capped = parser.parse(excessive_fields);
    check("partial field count is capped",
          fields_capped.partials.empty() &&
          fields_capped.has_truncation(TruncationReason::PARTIAL_FIELDS));

    const auto oversized_field = parser.parse(
        "`{:field.mu`1`" +
        std::string(DocumentParser::MAX_PARTIAL_FIELD_BYTES + 1, 'f') + "}");
    check("partial field bytes are capped",
          oversized_field.partials.empty() &&
          oversized_field.has_truncation(TruncationReason::PARTIAL_FIELD_BYTES));

    std::string total_metadata;
    for (std::size_t index = 0; index < DocumentParser::MAX_PARTIALS; ++index)
        total_metadata += "`{:" + std::string(256, static_cast<char>('a' + index)) + "}\n";
    const auto metadata_capped = parser.parse(total_metadata);
    check("aggregate partial metadata remains bounded",
          metadata_capped.partials.size() < DocumentParser::MAX_PARTIALS &&
          metadata_capped.has_truncation(TruncationReason::PARTIAL_DESCRIPTOR_BYTES));

    CompactPage compact;
    check("partial descriptors survive compact-page assignment", compact.assign(document) &&
          compact.partials().size() == 1 && compact.blocks().size() == 3 &&
          compact.blocks()[1].partial_index == 0);
    if (compact.partials().size() == 1) {
        const auto& partial = compact.partials()[0];
        check("compact partial strings and fields remain exact",
              compact.partial_url(partial) ==
                  "f64a846313b874ee4a357040807f8c77:/page/hello.mu" &&
              compact.partial_selectors(partial) == "pid=32|user_name" &&
              hex(partial.descriptor_hash) ==
                  "412d0e39b4703ab0f4b29f77158f252a66f560207ea5ea83882e270b8e68ea07" &&
              compact.partial_id(partial) == "32" && partial.field_count == 2 &&
              compact.partial_field(partial, 1) == "user_name");
    }

    const auto scheduled_document = parser.parse(
        "`{:first.mu`10}\n`{:second.mu`20}");
    CompactPage scheduled_page;
    check("scheduler fixture compacts", scheduled_page.assign(scheduled_document));
    PartialScheduler scheduler;
    check("scheduler state has a fixed constrained-memory footprint",
          sizeof(scheduler) <= 1024U);
    scheduler.configure(scheduled_page, 7U, 1000U);
    PartialRequest first{};
    PartialRequest blocked{};
    check("first partial is immediately due",
          scheduler.poll(1000U, true, true, first) && first.partial_index == 0 &&
          first.page_generation == 7U && first.request_token != 0U);
    check("only one browser request can be outstanding",
          !scheduler.poll(1000U, true, true, blocked));
    check("matching completion is accepted", scheduler.complete(first, true, 1100U));
    PartialRequest second{};
    check("second initial partial is serialized after first",
          scheduler.poll(1100U, true, true, second) && second.partial_index == 1);
    check("second completion is accepted", scheduler.complete(second, true, 1200U));
    check("refresh is based on request-start time and canonical strict expiry",
          !scheduler.poll(11000U, true, true, blocked) &&
          scheduler.poll(11001U, true, true, blocked) && blocked.partial_index == 0);

    PartialScheduler fair_scheduler;
    const auto fair_page = parser.parse("`{:first.mu`1}\n`{:second.mu`1}");
    fair_scheduler.configure(fair_page, 77U, 0U);
    PartialRequest fair_first;
    PartialRequest fair_second;
    check("overdue first partial cannot starve later due partials", [&] {
        return fair_scheduler.poll(0U, true, true, fair_first) &&
            fair_first.partial_index == 0 &&
            fair_scheduler.complete(fair_first, true, 2000U) &&
            fair_scheduler.poll(2000U, true, true, fair_second) &&
            fair_second.partial_index == 1;
    }());

    scheduler.configure(scheduled_page, 8U, 20000U);
    check("old-generation completion cannot mutate a new page",
          !scheduler.complete(blocked, true, 20001U));
    check("hidden browser suppresses dispatch",
          !scheduler.poll(20000U, false, true, blocked));
    check("busy browser owner suppresses dispatch",
          !scheduler.poll(20000U, true, false, blocked));
    check("dispatch resumes once browser is active and idle",
          scheduler.poll(20000U, true, true, blocked) &&
          blocked.page_generation == 8U);
    scheduler.cancel(8U);
    check("navigation cancellation revokes in-flight work",
          !scheduler.complete(blocked, true, 20001U) && scheduler.empty());

    const auto retry_document = parser.parse("`{:once.mu}");
    CompactPage retry_page;
    check("retry fixture compacts", retry_page.assign(retry_document));
    scheduler.configure(retry_page, 9U, 0U);
    PartialRequest retry{};
    check("one-shot partial starts immediately", scheduler.poll(0U, true, true, retry));
    const uint32_t first_partial_generation = retry.partial_generation;
    check("failure is contained", scheduler.complete(retry, false, 1U));
    check("one-shot transfer failure has no automatic retry",
          !scheduler.poll(0xffffffffU, true, true, retry));
    check("manual p-link style retry rearms the exact occurrence",
          scheduler.request_now(0U, 9U, 5000U) &&
          scheduler.poll(5000U, true, true, retry) &&
          retry.partial_generation != first_partial_generation);
    check("mismatched partial generation is rejected", [&] {
        PartialRequest stale = retry;
        --stale.partial_generation;
        return !scheduler.complete(stale, true, 5001U);
    }());
    check("mismatched descriptor identity is rejected", [&] {
        PartialRequest stale = retry;
        stale.descriptor_hash[0] ^= 0xffU;
        return !scheduler.complete(stale, true, 5001U);
    }());
    check("manual retry completion remains valid", scheduler.complete(retry, true, 5001U));

    const auto wrap_document = parser.parse("`{:wrap.mu`5}");
    CompactPage wrap_page;
    check("wrap fixture compacts", wrap_page.assign(wrap_document));
    scheduler.configure(wrap_page, 10U, 0xfffffff0U);
    check("initial request remains due across clock wrap",
          scheduler.poll(0xfffffff0U, true, true, retry));
    check("wrapped request failure is accepted", scheduler.complete(retry, false, 0xfffffff1U));
    check("wrap-safe deadline is not early",
          !scheduler.poll(0x00001379U, true, true, retry));
    check("wrap-safe deadline becomes due exactly once",
          scheduler.poll(0x0000137aU, true, true, retry));

    if (failures == 0) std::cout << "partial core parser checks passed\n";
    return failures == 0 ? 0 : 1;
}
