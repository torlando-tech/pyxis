#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "NavigationStack.h"
#include "NomadNetDocument.h"
#include "NomadNetDisplay.h"
#include "NomadNetHistory.h"
#include "NomadNetMailbox.h"
#include "NomadNetProtocol.h"
#include "NomadNetUrl.h"

using UI::LXMF::NavigationStack;
using UI::LXMF::Route;
using UI::LXMF::NomadNet::Alignment;
using UI::LXMF::NomadNet::BlockType;
using UI::LXMF::NomadNet::DocumentParser;
using UI::LXMF::NomadNet::compact_address;
using UI::LXMF::NomadNet::PageHistory;
using UI::LXMF::NomadNet::AsyncMailbox;
using UI::LXMF::NomadNet::ResponseBuffer;
using UI::LXMF::NomadNet::Url;

int main(int argc, char** argv) {
    int passed = 0;
    int failed = 0;
    auto check = [&](const char* name, bool condition) {
        if (condition) ++passed;
        else { ++failed; std::cerr << "FAIL: " << name << "\n"; }
    };

    NavigationStack nav;
    check("launcher is navigation root", nav.current() == Route::HOME && nav.depth() == 0);
    nav.navigate(Route::MESSAGES);
    nav.navigate(Route::CHAT);
    check("routes form a hierarchy", nav.current() == Route::CHAT && nav.depth() == 2);
    check("back restores parent route exactly once", nav.back() && nav.current() == Route::MESSAGES && nav.depth() == 1);
    nav.home();
    check("home clears hierarchy", nav.current() == Route::HOME && nav.depth() == 0);
    nav.navigate(Route::MESSAGES);
    nav.navigate(Route::COMPOSE);
    nav.replace(Route::CHAT);
    check("compose send replaces route", nav.current() == Route::CHAT && nav.back() && nav.current() == Route::MESSAGES);
    nav.navigate(Route::CHAT);
    nav.navigate(Route::CALL);
    check("call exit restores owner", nav.back() && nav.current() == Route::CHAT);
    nav.home();
    for (std::size_t i = 0; i < NavigationStack::MAX_DEPTH + 5; ++i)
        nav.navigate((i & 1) ? Route::STATUS : Route::NETWORK);
    check("navigation stack is bounded", nav.depth() == NavigationStack::MAX_DEPTH);
    for (std::size_t i = 0; i < NavigationStack::MAX_DEPTH; ++i) nav.back();
    check("bounded stack preserves root", nav.current() == Route::HOME && !nav.back());

    PageHistory history;
    history.open("a");
    history.open("b");
    history.reload();
    check("reload does not add browser history", history.current() == "b" && history.depth() == 1);
    check("browser back restores prior page", history.back() && history.current() == "a" && history.depth() == 0);
    for (std::size_t i = 0; i < PageHistory::MAX_DEPTH + 4; ++i) history.open(std::to_string(i));
    check("browser history is bounded", history.depth() == PageHistory::MAX_DEPTH);

    AsyncMailbox mailbox;
    const std::vector<uint8_t> old_link{1}, new_link{2};
    mailbox.begin(new_link);
    check("stale link callback is rejected", !mailbox.publish_link(old_link, true));
    check("active link callback is accepted", mailbox.publish_link(new_link, true));
    AsyncMailbox::Event event;
    check("link event crosses mailbox", mailbox.take(event) && event.kind == AsyncMailbox::Kind::LINK_ESTABLISHED);
    AsyncMailbox early_link;
    check("link callback may arrive before token arming", early_link.publish_link(new_link, true));
    early_link.begin(new_link);
    check("matching early link callback is retained", early_link.take(event) && event.kind == AsyncMailbox::Kind::LINK_ESTABLISHED);
    const std::vector<uint8_t> old_request{3}, new_request{4};
    AsyncMailbox early_request;
    early_request.begin(new_link);
    const uint8_t early[] = {'o', 'k'};
    check("response may arrive before receipt token arming",
          early_request.publish_response(new_request, early, sizeof(early), sizeof(early)));
    early_request.expect_request(new_request);
    check("matching early response is retained",
          early_request.take(event) && event.kind == AsyncMailbox::Kind::RESPONSE);
    mailbox.expect_request(new_request);
    const uint8_t tiny[] = {0xc4, 1, 'x'};
    check("stale response callback is rejected", !mailbox.publish_response(old_request, tiny, sizeof(tiny), sizeof(tiny)));
    check("active response callback is accepted", mailbox.publish_response(new_request, tiny, sizeof(tiny), sizeof(tiny)));
    check("terminal response is not overwritten by a racing link-close callback",
          !mailbox.publish_link(new_link, false));
    check("response event crosses mailbox", mailbox.take(event) && event.kind == AsyncMailbox::Kind::RESPONSE && event.data.size() == sizeof(tiny));
    mailbox.expect_request(new_request);
    check("oversized transfer is rejected before payload retention",
          mailbox.publish_progress(new_request, AsyncMailbox::MAX_WIRE_BYTES + 1) && mailbox.take(event) &&
          event.kind == AsyncMailbox::Kind::OVERSIZED && event.data.empty());

    Url url;
    std::string error;
    check("32 hex destination with page path parses",
          Url::parse("0123456789abcdef0123456789ABCDEF:/page/index.mu", url, error));
    check("url normalizes destination hex and preserves path",
          url.destination_hex == "0123456789abcdef0123456789abcdef" && url.path == "/page/index.mu");
    check("bare destination gets default page",
          Url::parse("0123456789abcdef0123456789abcdef", url, error) && url.path == "/page/index.mu");
    check("relative same-node path parses with context",
          Url::parse(":/page/about.mu", url, error, "fedcba9876543210fedcba9876543210") &&
          url.destination_hex == "fedcba9876543210fedcba9876543210");
    check("wrong destination length rejected", !Url::parse("abcd:/page/index.mu", url, error));
    check("nonhex destination rejected", !Url::parse("zz23456789abcdef0123456789abcdef:/page/index.mu", url, error));
    check("control characters rejected", !Url::parse("0123456789abcdef0123456789abcdef:/page/a\nb", url, error));
    check("downloads explicitly unsupported", !Url::parse("0123456789abcdef0123456789abcdef:/file/x", url, error));

    DocumentParser parser;
    auto doc = parser.parse(
        "#!c=60\n#!bg=123\n#!fg=abcdef\n>> Heading\n"
        "ordinary `!bold`! `*italic`* `_under`_ `Ff00red`f `B0f0back`b `ccentre\n"
        "`[Next`0123456789abcdef0123456789abcdef:/page/next.mu]\n-\n"
        "`=\n`!literal\n\\`=\n`=\n`tabc\n");
    check("cache metadata parsed from first line", doc.cache_seconds == 60);
    check("page colors parsed", doc.has_background && doc.background == 0x112233 &&
                                      doc.has_foreground && doc.foreground == 0xabcdef);
    check("heading depth parsed", doc.blocks.size() >= 5 &&
                                  doc.blocks[0].type == BlockType::HEADING && doc.blocks[0].depth == 2);
    check("inline runs are styled", doc.blocks[1].runs.size() >= 6 &&
                                      doc.blocks[1].runs[1].bold && doc.blocks[1].runs[3].italic);
    bool saw_background = false;
    for (const auto& run : doc.blocks[1].runs) saw_background = saw_background || run.has_background;
    check("inline background style parsed", saw_background);
    check("alignment modifier represented", doc.blocks[1].alignment == Alignment::CENTER);
    check("links are model elements", doc.links.size() == 1 &&
                                       doc.links[0].target.find("/page/next.mu") != std::string::npos);
    auto link_fields = parser.parse("`[Search`:/page/search.mu`q=pyxis]\n");
    check("link target excludes request fields", link_fields.links.size() == 1 &&
          link_fields.links[0].target == ":/page/search.mu" && link_fields.links[0].fields == "q=pyxis");
    check("divider parsed", doc.blocks[3].type == BlockType::DIVIDER);
    check("literal mode suppresses formatting", doc.blocks[4].runs.size() == 1 &&
                                                 doc.blocks[4].runs[0].text == "`!literal");
    bool saw_unsupported = false;
    for (const auto& block : doc.blocks) saw_unsupported = saw_unsupported || block.type == BlockType::UNSUPPORTED;
    check("unsupported structured content has fallback", doc.unsupported && saw_unsupported);

    auto later_cache = parser.parse("text\n#!c=99999999999999999999\nmore");
    check("cache metadata is first-line-only", later_cache.cache_seconds == 0);
    auto clamped_cache = parser.parse("#!c=99999999999999999999\ntext");
    check("cache seconds clamps without overflow", clamped_cache.cache_seconds == DocumentParser::MAX_CACHE_SECONDS);
    auto high_bit_digit = parser.parse(std::string("#!c=1") + char(0xff) + "\ntext");
    check("cache digit validation is unsigned-char safe", high_bit_digit.cache_seconds == 0 && high_bit_digit.malformed);

    std::string invalid_utf8("ok\xF0\x28\x8C\x28", 6);
    auto utf8_doc = parser.parse(invalid_utf8);
    check("invalid UTF-8 is rejected before rendering", utf8_doc.malformed && utf8_doc.blocks.empty());

    std::string huge(DocumentParser::MAX_DOCUMENT_BYTES, 'x');
    huge += "\n#!c=99\n";
    auto huge_doc = parser.parse(huge);
    check("document bytes capped and truncation propagated", huge_doc.truncated &&
                                                             huge_doc.source_bytes == DocumentParser::MAX_DOCUMENT_BYTES);
    check("parser never processes metadata beyond retained source", huge_doc.cache_seconds == 0);
    std::string long_line(DocumentParser::MAX_SOURCE_LINE_BYTES + 20, 'q');
    auto line_doc = parser.parse(long_line);
    check("source line capped", line_doc.truncated && !line_doc.blocks.empty() &&
                                line_doc.blocks[0].runs[0].text.size() == DocumentParser::MAX_SOURCE_LINE_BYTES);
    auto utf8_boundary = parser.parse(std::string(DocumentParser::MAX_SOURCE_LINE_BYTES - 1, 'x') + "\xe2\x82\xac");
    check("line truncation preserves UTF-8 boundaries", utf8_boundary.truncated && !utf8_boundary.malformed &&
                                                        utf8_boundary.blocks[0].runs[0].text.size() ==
                                                            DocumentParser::MAX_SOURCE_LINE_BYTES - 1);
    auto blank_lines = parser.parse("one\n\nthree\n");
    check("blank lines preserve vertical structure", blank_lines.blocks.size() == 4 &&
                                                      blank_lines.blocks[1].runs[0].text == " ");
    std::string many_lines;
    for (std::size_t i = 0; i < DocumentParser::MAX_SOURCE_LINES + 50; ++i) many_lines += "x\n";
    auto lines_doc = parser.parse(many_lines);
    check("source line count capped", lines_doc.truncated &&
                                      lines_doc.source_lines <= DocumentParser::MAX_SOURCE_LINES);
    std::string many_runs;
    for (std::size_t i = 0; i < DocumentParser::MAX_RUNS_PER_LINE + 20; ++i) many_runs += "x`!";
    auto runs_doc = parser.parse(many_runs);
    check("runs per line capped", runs_doc.truncated &&
                                    runs_doc.blocks[0].runs.size() == DocumentParser::MAX_RUNS_PER_LINE);
    std::string global_runs;
    for (std::size_t i = 0; i < DocumentParser::MAX_TOTAL_RUNS; ++i) global_runs += "a`!b`!\n";
    auto global_runs_doc = parser.parse(global_runs);
    std::size_t retained_runs = 0;
    for (const auto& block : global_runs_doc.blocks) retained_runs += block.runs.size();
    check("runs are globally bounded", global_runs_doc.truncated && retained_runs <= DocumentParser::MAX_TOTAL_RUNS);
    std::string many_links;
    for (std::size_t i = 0; i < DocumentParser::MAX_LINKS + 10; ++i) many_links += "`[x`:/page/x]";
    auto links_doc = parser.parse(many_links);
    check("links are globally bounded", links_doc.truncated && links_doc.links.size() == DocumentParser::MAX_LINKS);

    auto malformed = parser.parse("#!c=not-a-number\n#!bg=xyz\n`F1broken\n[label`target\n`=\ntext");
    check("malformed micron remains bounded", malformed.blocks.size() <= DocumentParser::MAX_BLOCKS);
    check("invalid metadata ignored", malformed.cache_seconds == 0 && !malformed.has_background);
    check("unterminated literal is reported", malformed.malformed);

    if (argc > 1) {
        std::ifstream input(argv[1], std::ios::binary);
        std::string fixture((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto real = parser.parse(fixture);
        check("authoritative Aleph fixture is readable", input.good() || input.eof());
        check("authoritative fixture yields headings", !real.blocks.empty() && real.blocks[0].type == BlockType::HEADING);
        check("authoritative fixture yields links", !real.links.empty());
        check("authoritative fixture remains within bounds", real.blocks.size() <= DocumentParser::MAX_BLOCKS &&
                                                             real.links.size() <= DocumentParser::MAX_LINKS);
    } else {
        check("authoritative Aleph fixture is readable", false);
        check("authoritative fixture yields headings", false);
        check("authoritative fixture yields links", false);
        check("authoritative fixture remains within bounds", false);
    }

    const auto nil = UI::LXMF::NomadNet::no_form_request_data();
    check("no-form request is exact msgpack nil", nil.size() == 1 && nil[0] == 0xc0);
    ResponseBuffer response;
    const uint8_t bin8[] = {0xc4, 0x03, 'm', 'u', '!'};
    check("msgpack bin8 response normalizes", UI::LXMF::NomadNet::normalize_response(bin8, sizeof(bin8), response) &&
                                               response.size() == 3 && response.bytes()[0] == 'm');
    const uint8_t bin16[] = {0xc5, 0x00, 0x03, 'm', 'u', '!'};
    check("msgpack bin16 response normalizes", UI::LXMF::NomadNet::normalize_response(bin16, sizeof(bin16), response));
    std::vector<uint8_t> exact(5 + ResponseBuffer::MAX_BYTES, 'x');
    exact[0] = 0xc6; exact[1] = 0; exact[2] = 1; exact[3] = 0; exact[4] = 0;
    check("exact 64KiB bin32 is accepted", UI::LXMF::NomadNet::normalize_response(exact.data(), exact.size(), response) &&
                                             response.size() == ResponseBuffer::MAX_BYTES && !response.truncated());
    const uint8_t raw[] = {'r', 'a', 'w'};
    check("raw packet response normalizes", UI::LXMF::NomadNet::normalize_response(raw, sizeof(raw), response) &&
                                             response.size() == sizeof(raw) && response.bytes()[0] == 'r');
    const uint8_t trailing[] = {0xc4, 0x01, 'x', 'y'};
    check("trailing bytes are rejected", !UI::LXMF::NomadNet::normalize_response(trailing, sizeof(trailing), response));
    const uint8_t malformed8[] = {0xc4};
    check("malformed bin8 is rejected", !UI::LXMF::NomadNet::normalize_response(malformed8, sizeof(malformed8), response));
    const uint8_t malformed16[] = {0xc5, 0x00};
    check("malformed bin16 is rejected", !UI::LXMF::NomadNet::normalize_response(malformed16, sizeof(malformed16), response));
    const uint8_t malformed32[] = {0xc6, 0, 1, 0};
    check("malformed bin32 is rejected", !UI::LXMF::NomadNet::normalize_response(malformed32, sizeof(malformed32), response));
    const uint8_t oversize_advertised[] = {0xc6, 0, 1, 0, 1};
    check("oversize advertised payload is rejected before retention",
          !UI::LXMF::NomadNet::normalize_response(oversize_advertised, sizeof(oversize_advertised), response) && response.size() == 0);
    check("null and empty responses are rejected", !UI::LXMF::NomadNet::normalize_response(nullptr, 0, response));

    check("compact address preserves short values", compact_address("node:/page/a.mu", 32) == "node:/page/a.mu");
    check("compact address abbreviates destination but preserves path",
          compact_address("a8d24177d946de4f1f0a0fe1af9a1338:/page/index.mu", 30) ==
              "a8d24177...1338 /page/index.mu");
    check("compact address uses compiled-font-safe ASCII",
          compact_address("a8d24177d946de4f1f0a0fe1af9a1338:/page/index.mu", 30).find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/._- ") == std::string::npos);
    check("compact address remains bounded for long paths",
          compact_address("a8d24177d946de4f1f0a0fe1af9a1338:/page/a-very-long-page-name.mu", 24).size() <= 24);

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
