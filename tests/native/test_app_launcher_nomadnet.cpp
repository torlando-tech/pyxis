#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "NavigationStack.h"
#include "NomadNetDocument.h"
#include "NomadNetDisplay.h"
#include "NomadNetHistory.h"
#include "NomadNetGlyphs.h"
#include "NomadNetLibrary.h"
#include "NomadNetActionMailbox.h"
#include "NomadNetMailbox.h"
#include "NomadNetCompactPage.h"
#include "NomadNetColors.h"
#include "NomadNetFocus.h"
#include "NomadNetProtocol.h"
#include "NomadNetRequestPolicy.h"
#include "NomadNetUrl.h"
#include "NomadNetVirtualViewport.h"

using UI::LXMF::NavigationStack;
using UI::LXMF::Route;
using UI::LXMF::NomadNet::Alignment;
using UI::LXMF::NomadNet::BlockType;
using UI::LXMF::NomadNet::DocumentParser;
using UI::LXMF::NomadNet::TruncationReason;
using UI::LXMF::NomadNet::compact_address;
using UI::LXMF::NomadNet::PageHistory;
using UI::LXMF::NomadNet::display_text;
using UI::LXMF::NomadNet::display_codepoint;
using UI::LXMF::NomadNet::Library;
using UI::LXMF::NomadNet::ActionMailbox;
using UI::LXMF::NomadNet::UserAction;
using UI::LXMF::NomadNet::UserActionKind;
using UI::LXMF::NomadNet::sanitize_directory_name;
using UI::LXMF::NomadNet::page_title;
using UI::LXMF::NomadNet::AsyncMailbox;
using UI::LXMF::NomadNet::CompactPage;
using UI::LXMF::NomadNet::resolve_foreground;
using UI::LXMF::NomadNet::for_each_focus_span;
using UI::LXMF::NomadNet::ResponseBuffer;
using UI::LXMF::NomadNet::RequestPolicy;
using UI::LXMF::NomadNet::Url;
using UI::LXMF::NomadNet::VirtualViewport;

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
    history.open("b", true, 84);
    history.reload();
    check("reload does not add browser history", history.current() == "b" && history.depth() == 1);
    check("browser back restores prior page and logical scroll",
          history.back() && history.current() == "a" && history.current_scroll() == 84 &&
              history.depth() == 0);
    history.clear();
    history.open("node:/page/a.mu");
    history.open("node:/page/a.mu#details", true, 137);
    check("anchor navigation creates meaningful history entry",
          history.depth() == 1 && history.current() == "node:/page/a.mu#details");
    check("anchor back restores exact prior position",
          history.back() && history.current() == "node:/page/a.mu" &&
              history.current_scroll() == 137);
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
    mailbox.seal();
    check("terminal cleanup rejects its synthetic failed callback",
          !mailbox.publish_failed(new_request));
    mailbox.prepare();
    check("next operation still accepts an early link callback",
          mailbox.publish_link(new_link, true));
    check("prepared early link event crosses mailbox",
          mailbox.take(event) && event.kind == AsyncMailbox::Kind::LINK_ESTABLISHED);
    mailbox.expect_request(new_request);
    check("oversized response is rejected before payload retention",
          mailbox.publish_oversized(new_request, AsyncMailbox::MAX_WIRE_BYTES + 1) && mailbox.take(event) &&
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
    check("link fields are separated from the registered request path",
          Url::parse(":/page/repo.mu`g=reticulum|r=lxmf", url, error,
                     "fedcba9876543210fedcba9876543210") &&
          url.path == "/page/repo.mu" && url.fields == "g=reticulum|r=lxmf");
    check("canonical URL preserves link fields for history and reload",
          url.str() == "fedcba9876543210fedcba9876543210:/page/repo.mu`g=reticulum|r=lxmf");
    check("page fragment is separated from transport request path",
          Url::parse("fedcba9876543210fedcba9876543210:/page/guide.mu#anchors`mode=full",
                     url, error) &&
              url.path == "/page/guide.mu" && url.has_fragment &&
              url.fragment == "anchors" && url.fields == "mode=full");
    check("canonical URL retains fragment before link fields",
          url.str() == "fedcba9876543210fedcba9876543210:/page/guide.mu#anchors`mode=full");
    Url current_url = url;
    Url fragment_url;
    check("fragment-only target inherits the complete current resource",
          Url::parse("#next-section", fragment_url, error, current_url.destination_hex,
                     current_url.path, current_url.fields) &&
              fragment_url.destination_hex == current_url.destination_hex &&
              fragment_url.path == current_url.path &&
              fragment_url.fields == current_url.fields && fragment_url.has_fragment &&
              fragment_url.fragment == "next-section");
    check("empty fragment is retained for canonical next-heading navigation",
          Url::parse("#", fragment_url, error, current_url.destination_hex,
                     current_url.path, current_url.fields) &&
              fragment_url.has_fragment && fragment_url.fragment.empty());
    check("fragment navigation keeps transport resource identity",
          current_url.same_resource(fragment_url));
    check("fragment navigation is local only with a loaded matching resource",
          UI::LXMF::NomadNet::should_jump_locally(current_url, fragment_url, true, false) &&
              !UI::LXMF::NomadNet::should_jump_locally(current_url, fragment_url, false, false));
    Url other_page;
    check("fragment on another path still requires transport",
          Url::parse(":/page/other.mu#next-section", other_page, error,
                     current_url.destination_hex, current_url.path, current_url.fields) &&
              !UI::LXMF::NomadNet::should_jump_locally(current_url, other_page, true, false));
    check("history restoration may reuse the loaded matching resource",
          UI::LXMF::NomadNet::should_jump_locally(current_url, current_url, true, true));
    check("fragment names reject non-anchor grammar",
          !Url::parse("#bad/name", fragment_url, error, current_url.destination_hex,
                      current_url.path, current_url.fields));
    check("fragment names are bounded",
          !Url::parse("#" + std::string(Url::MAX_FRAGMENT_BYTES + 1, 'a'),
                      fragment_url, error, current_url.destination_hex,
                      current_url.path, current_url.fields));
    check("wrong destination length rejected", !Url::parse("abcd:/page/index.mu", url, error));
    check("nonhex destination rejected", !Url::parse("zz23456789abcdef0123456789abcdef:/page/index.mu", url, error));
    check("control characters rejected", !Url::parse("0123456789abcdef0123456789abcdef:/page/a\nb", url, error));
    check("downloads explicitly unsupported", !Url::parse("0123456789abcdef0123456789abcdef:/file/x", url, error));

    DocumentParser parser;
    auto block_text = [](const UI::LXMF::NomadNet::Block& block) {
        std::string value;
        for (const auto& run : block.runs) value += run.text;
        return value;
    };
    auto unknown_modifier = parser.parse("before `xafter");
    check("unknown modifier is consumed like canonical NomadNet",
          unknown_modifier.blocks.size() == 1 && block_text(unknown_modifier.blocks[0]) == "before after");

    auto trailing_introducer = parser.parse("before `");
    check("trailing formatting introducer is consumed",
          trailing_introducer.blocks.size() == 1 && block_text(trailing_introducer.blocks[0]) == "before ");

    auto escaped_backtick = parser.parse("before \\`after");
    check("escaped backtick remains visible",
          escaped_backtick.blocks.size() == 1 && block_text(escaped_backtick.blocks[0]) == "before `after");

    auto consecutive_unknown = parser.parse("`x`ytext");
    check("consecutive unknown modifiers are consumed independently",
          consecutive_unknown.blocks.size() == 1 && block_text(consecutive_unknown.blocks[0]) == "text");

    auto unicode_unknown = parser.parse(u8"`§two `☃three `🙂four");
    check("unknown Unicode modifiers consume one complete codepoint",
          unicode_unknown.blocks.size() == 1 && block_text(unicode_unknown.blocks[0]) == "two three four");

    auto unknown_then_bold = parser.parse("`x`!bold`! plain");
    bool unknown_preserves_formatting = false;
    if (unknown_then_bold.blocks.size() == 1) {
        for (const auto& run : unknown_then_bold.blocks[0].runs) {
            if (run.text == "bold" && run.bold) unknown_preserves_formatting = true;
        }
    }
    check("unknown modifier does not change formatting state",
          unknown_then_bold.blocks.size() == 1 && block_text(unknown_then_bold.blocks[0]) == "bold plain" &&
              unknown_preserves_formatting);

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
    check("heading level one uses the large face",
          UI::LXMF::NomadNet::heading_uses_large_font(1));
    check("nested headings use the body-size faces",
          !UI::LXMF::NomadNet::heading_uses_large_font(2) &&
              !UI::LXMF::NomadNet::heading_uses_large_font(255));
    check("empty headings remain layout content",
          UI::LXMF::NomadNet::block_has_layout_content(BlockType::HEADING, 0));
    check("dark heading one colors match NomadNet",
          UI::LXMF::NomadNet::heading_foreground(1) == 0x222222 &&
              UI::LXMF::NomadNet::heading_background(1) == 0xbbbbbb);
    check("dark heading two colors match NomadNet",
          UI::LXMF::NomadNet::heading_foreground(2) == 0x111111 &&
              UI::LXMF::NomadNet::heading_background(2) == 0x999999);
    check("dark heading three and deeper colors match NomadNet",
          UI::LXMF::NomadNet::heading_foreground(3) == 0x000000 &&
              UI::LXMF::NomadNet::heading_background(3) == 0x777777 &&
              UI::LXMF::NomadNet::heading_foreground(255) == 0x000000 &&
              UI::LXMF::NomadNet::heading_background(255) == 0x777777);
    check("heading indentation follows depth with a bounded cap",
          UI::LXMF::NomadNet::heading_indent_spaces(1) == 0 &&
              UI::LXMF::NomadNet::heading_indent_spaces(2) == 2 &&
              UI::LXMF::NomadNet::heading_indent_spaces(3) == 4 &&
              UI::LXMF::NomadNet::heading_indent_spaces(255) == 14);
    check("heading spacing distinguishes canonical levels",
          UI::LXMF::NomadNet::heading_bottom_spacing(1) == 6 &&
              UI::LXMF::NomadNet::heading_bottom_spacing(2) == 4 &&
              UI::LXMF::NomadNet::heading_bottom_spacing(3) == 3 &&
              UI::LXMF::NomadNet::heading_bottom_spacing(255) == 3);
    auto authored_heading = parser.parse("> `F0f0green heading`f");
    CompactPage authored_heading_page;
    bool authored_heading_assigned = authored_heading_page.assign(authored_heading);
    bool authored_heading_foreground = false;
    for (const auto& run : authored_heading_page.runs()) {
        authored_heading_foreground = authored_heading_foreground ||
            ((run.style & CompactPage::HAS_FOREGROUND) && run.foreground == 0x00ff00);
    }
    check("heading treatment preserves authored foreground",
          authored_heading_assigned && authored_heading_page.blocks().size() == 1 &&
              authored_heading_foreground);
    auto canonical_heading = parser.parse("#!fg=f00\n#!bg=00f\n> plain `F0f0green`f `Bf80orange background`b");
    CompactPage canonical_heading_page;
    bool heading_defaults_override_page = false;
    bool authored_heading_fg_override = false;
    bool authored_heading_bg_override = false;
    if (canonical_heading_page.assign(canonical_heading)) {
        for (const auto& run : canonical_heading_page.runs()) {
            const auto run_text = canonical_heading_page.text(run);
            const std::string run_string(run_text.data(), run_text.size());
            if (run_string.find("plain") != std::string::npos) {
                heading_defaults_override_page =
                    UI::LXMF::NomadNet::resolve_effective_foreground(
                        canonical_heading_page, run, 1, 0xffffff) == 0x222222 &&
                    UI::LXMF::NomadNet::resolve_effective_background(
                        canonical_heading_page, run, 1, 0x000000) == 0xbbbbbb;
            }
            if (run_string.find("green") != std::string::npos) {
                authored_heading_fg_override =
                    UI::LXMF::NomadNet::resolve_effective_foreground(
                        canonical_heading_page, run, 1, 0xffffff) == 0x00ff00;
            }
            if (run_string.find("orange background") != std::string::npos) {
                authored_heading_bg_override =
                    UI::LXMF::NomadNet::resolve_effective_background(
                        canonical_heading_page, run, 1, 0x000000) == 0xff8800;
            }
        }
    }
    check("heading defaults override page colors", heading_defaults_override_page);
    check("authored heading foreground overrides heading default", authored_heading_fg_override);
    check("authored heading background overrides heading default", authored_heading_bg_override);
    CompactPage::RunRecord unstyled_heading_run{};
    check("heading focus contrast uses heading background instead of page background",
          UI::LXMF::NomadNet::resolve_focus_border(
              canonical_heading_page, unstyled_heading_run, 0x000000, 1) == 0x000000);
    auto empty_heading = parser.parse(">\nbody");
    CompactPage empty_heading_page;
    check("empty heading survives compact conversion for its solid band",
          empty_heading_page.assign(empty_heading) && !empty_heading_page.blocks().empty() &&
              empty_heading_page.blocks()[0].type == BlockType::HEADING &&
              empty_heading_page.blocks()[0].run_count == 0);
    check("inline runs are styled", doc.blocks[1].runs.size() >= 6 &&
                                      doc.blocks[1].runs[1].bold && doc.blocks[1].runs[3].italic);
    bool saw_background = false;
    for (const auto& run : doc.blocks[1].runs) saw_background = saw_background || run.has_background;
    check("inline background style parsed", saw_background);
    check("alignment modifier represented", doc.blocks[1].alignment == Alignment::CENTER);
    check("links are model elements", doc.links.size() == 1 &&
                                       doc.links[0].target.find("/page/next.mu") != std::string::npos);
    CompactPage compact;
    check("compact page owns one bounded text arena", compact.assign(doc) &&
          compact.arena_bytes() > 0 && compact.blocks().size() == doc.blocks.size() &&
          compact.runs().size() <= CompactPage::MAX_RUNS && compact.links().size() == doc.links.size());
    check("compact runs address text by arena offset", !compact.runs().empty() &&
          std::string(compact.text(compact.runs().front()).data(), compact.text(compact.runs().front()).size()) ==
              doc.blocks.front().runs.front().text);
    check("compact links address targets without renderer copies", !compact.links().empty() &&
          std::string(compact.target(0).data(), compact.target(0).size()) == doc.links.front().target);
    check("compact page preserves document presentation flags",
          compact.has_background() == doc.has_background && compact.background() == doc.background &&
          compact.has_foreground() == doc.has_foreground && compact.foreground() == doc.foreground);

    const auto anchor_doc = parser.parse(
        "First\n"
        "`:spot Second\n"
        "`:spot Duplicate\n"
        "> Styled `!Heading`! Caf\xC3\xA9\n"
        "`:only\n"
        "`:mark!Visible punctuation\n");
    check("explicit anchors are retained at their zero-width block",
          anchor_doc.anchors.size() == 4 && anchor_doc.anchors[0].name == "spot" &&
              anchor_doc.anchors[0].block_index == 1);
    check("first duplicate anchor declaration wins",
          anchor_doc.anchors[0].block_index == 1);
    check("explicit anchor declaration does not render",
          anchor_doc.blocks[1].runs.size() == 1 && anchor_doc.blocks[1].runs[0].text == " Second");
    check("anchor grammar stops without consuming punctuation",
          anchor_doc.blocks[5].runs.size() == 1 &&
              anchor_doc.blocks[5].runs[0].text == "!Visible punctuation");
    check("anchor-only declaration remains zero width",
          anchor_doc.anchors[2].name == "only" && anchor_doc.anchors[2].block_index == 4 &&
              anchor_doc.blocks[4].runs.empty());
    check("heading slug strips Micron formatting and non-ASCII text",
          anchor_doc.anchors[1].name == "styled-heading-caf" &&
              anchor_doc.anchors[1].block_index == 3);
    const auto canonical_slug_doc = parser.parse(
        "> Hello, World!\n> A___B---C\n> A\xC3\xA9" "B\n"
        "> `[Label`:/page/target.mu] Link\n"
        "> `FabcRed `BT123456Blue\n> Bad `Fzzzx Color\n"
        "> `Fg50Gray`f `Bg25Shade`b\n");
    check("generated slugs collapse canonical separator runs",
          canonical_slug_doc.anchors[0].name == "hello-world" &&
              canonical_slug_doc.anchors[1].name == "a-b-c");
    check("generated slugs replace a Unicode run with one separator",
          canonical_slug_doc.anchors[2].name == "a-b");
    check("generated slugs include canonical link label and target text",
          canonical_slug_doc.anchors[3].name == "label-page-target-mu-link");
    check("generated slugs strip valid colors but retain malformed color text",
          canonical_slug_doc.anchors[4].name == "red-blue" &&
              canonical_slug_doc.anchors[5].name == "bad-fzzzx-color");
    check("generated slugs strip grayscale foreground and background modifiers",
          canonical_slug_doc.anchors[6].name == "gray-shade");
    check("explicit anchor names preserve canonical case sensitivity",
          parser.parse("`:Mixed_Name-2 target\n").anchors.front().name == "Mixed_Name-2");
    const auto empty_anchor_doc = parser.parse("`: visible\n");
    check("empty anchor declarations are ignored and remain zero width",
          empty_anchor_doc.anchors.empty() && empty_anchor_doc.blocks.front().runs.front().text == " visible");
    const std::string long_anchor(DocumentParser::MAX_ANCHOR_NAME_BYTES + 1, 'a');
    const auto long_anchor_doc = parser.parse("`:" + long_anchor + " visible\n");
    check("overlong anchors are consumed without retaining a false prefix",
          long_anchor_doc.anchors.empty() && long_anchor_doc.blocks.front().runs.front().text == " visible" &&
              long_anchor_doc.has_truncation(TruncationReason::ANCHOR_NAME_BYTES));
    const auto long_heading_anchor_doc = parser.parse("> " + long_anchor + "\n");
    check("overlong generated heading slugs do not retain a false prefix",
          long_heading_anchor_doc.anchors.empty() &&
              long_heading_anchor_doc.has_truncation(TruncationReason::ANCHOR_NAME_BYTES));
    std::string many_anchors_source;
    for (std::size_t i = 0; i <= DocumentParser::MAX_ANCHORS; ++i)
        many_anchors_source += "`:a" + std::to_string(i) + " x\n";
    const auto many_anchors_doc = parser.parse(many_anchors_source);
    check("anchor count is independently bounded",
          many_anchors_doc.anchors.size() == DocumentParser::MAX_ANCHORS &&
              many_anchors_doc.has_truncation(TruncationReason::ANCHORS));
    const auto duplicate_slug_doc = parser.parse(
        "`:same-heading explicit\n> Same `!Heading`!\n");
    check("explicit declaration wins over a later generated heading slug",
          duplicate_slug_doc.anchors.size() == 1 &&
              duplicate_slug_doc.anchors.front().block_index == 0);
    CompactPage anchor_page;
    uint16_t anchor_block = 0;
    check("compact page retains bounded anchor records",
          anchor_page.assign(anchor_doc) && anchor_page.anchors().size() == anchor_doc.anchors.size());
    check("compact anchor lookup resolves exact name to block",
          anchor_page.find_anchor("styled-heading-caf", anchor_block) && anchor_block == 3);
    check("compact anchor lookup is case-sensitive and allocation-free",
          !anchor_page.find_anchor("Styled-Heading-Caf", anchor_block));

    auto color_doc = parser.parse(
        "#!bg=123\n#!fg=abc\n"
        "plain `Ff00inline`f `B0f0back`b "
        "`F79d`_`[styled link`:/page/next.mu]`_`f\n");
    CompactPage color_page;
    check("Micron color fixture compacts", color_page.assign(color_doc) &&
                                             color_page.has_background() &&
                                             color_page.background() == 0x112233 &&
                                             color_page.has_foreground() &&
                                             color_page.foreground() == 0xaabbcc);
    bool saw_plain = false;
    bool saw_inline = false;
    bool saw_colored_background = false;
    bool saw_styled_link = false;
    for (const auto& run : color_page.runs()) {
        const auto view = color_page.text(run);
        const std::string text(view.data(), view.size());
        if (text == "plain ") {
            saw_plain = resolve_foreground(color_page, run, 0xe8b4f0) == 0xaabbcc;
        } else if (text == "inline") {
            saw_inline = resolve_foreground(color_page, run, 0xe8b4f0) == 0xff0000;
        } else if (text == "back") {
            saw_colored_background = (run.style & CompactPage::HAS_BACKGROUND) &&
                                     run.background == 0x00ff00;
        } else if (text == "styled link") {
            saw_styled_link = run.link_index >= 0 &&
                              resolve_foreground(color_page, run, 0xe8b4f0) == 0x7799dd;
        }
    }
    check("page foreground is the unstyled text default", saw_plain);
    check("inline foreground overrides the page default", saw_inline);
    check("inline background is preserved", saw_colored_background);
    check("links preserve their authored Micron foreground", saw_styled_link);

    auto truecolor_doc = parser.parse(
        "#!fg=102030\n#!bg=405060\n"
        "`FTa1b2c3true foreground`f `BTd4e5f6true background`b\n");
    check("six-digit page truecolor parses",
          truecolor_doc.has_foreground && truecolor_doc.foreground == 0x102030 &&
              truecolor_doc.has_background && truecolor_doc.background == 0x405060);
    bool saw_truecolor_foreground = false;
    bool saw_truecolor_background = false;
    for (const auto& run : truecolor_doc.blocks.front().runs) {
        if (run.text == "true foreground") {
            saw_truecolor_foreground = run.has_foreground && run.foreground == 0xa1b2c3;
        } else if (run.text == "true background") {
            saw_truecolor_background = run.has_background && run.background == 0xd4e5f6;
        }
    }
    check("FT inline truecolor parses", saw_truecolor_foreground);
    check("BT inline truecolor parses", saw_truecolor_background);

    // Byte-exact Urwid 2.6.16 AttrSpec g00..g99 truecolor expansion.
    static constexpr uint32_t grayscale_rgb[100] = {
        0x000000, 0x000000, 0x080808, 0x080808, 0x080808, 0x121212, 0x121212, 0x121212, 0x121212, 0x1c1c1c,
        0x1c1c1c, 0x1c1c1c, 0x1c1c1c, 0x262626, 0x262626, 0x262626, 0x262626, 0x303030, 0x303030, 0x303030,
        0x303030, 0x3a3a3a, 0x3a3a3a, 0x3a3a3a, 0x3a3a3a, 0x444444, 0x444444, 0x444444, 0x444444, 0x4e4e4e,
        0x4e4e4e, 0x4e4e4e, 0x4e4e4e, 0x585858, 0x585858, 0x585858, 0x585858, 0x626262, 0x626262, 0x626262,
        0x626262, 0x6c6c6c, 0x6c6c6c, 0x6c6c6c, 0x6c6c6c, 0x767676, 0x767676, 0x767676, 0x767676, 0x808080,
        0x808080, 0x848484, 0x848484, 0x848484, 0x848484, 0x949494, 0x949494, 0x949494, 0x949494, 0x949494,
        0x9e9e9e, 0x9e9e9e, 0x9e9e9e, 0x9e9e9e, 0xa8a8a8, 0xa8a8a8, 0xa8a8a8, 0xa8a8a8, 0xb2b2b2, 0xb2b2b2,
        0xb2b2b2, 0xb2b2b2, 0xbcbcbc, 0xbcbcbc, 0xbcbcbc, 0xbcbcbc, 0xc6c6c6, 0xc6c6c6, 0xc6c6c6, 0xc6c6c6,
        0xd0d0d0, 0xd0d0d0, 0xd0d0d0, 0xd0d0d0, 0xdadada, 0xdadada, 0xdadada, 0xdadada, 0xe4e4e4, 0xe4e4e4,
        0xe4e4e4, 0xe4e4e4, 0xeeeeee, 0xeeeeee, 0xeeeeee, 0xeeeeee, 0xeeeeee, 0xffffff, 0xffffff, 0xffffff,
    };
    for (std::size_t percent = 0; percent < 100; ++percent) {
        std::string token = "g00";
        token[1] = static_cast<char>('0' + percent / 10);
        token[2] = static_cast<char>('0' + percent % 10);
        const auto foreground_page = parser.parse(std::string("#!fg=") + token + "\ntext");
        const auto background_page = parser.parse(std::string("#!bg=") + token + "\ntext");
        const std::string name = std::string("page grayscale matches Urwid for ") + token;
        check(name.c_str(),
              foreground_page.has_foreground && foreground_page.foreground == grayscale_rgb[percent] &&
                  background_page.has_background && background_page.background == grayscale_rgb[percent] &&
                  !foreground_page.malformed && !background_page.malformed);
    }
    auto inline_grayscale = parser.parse("`Fg02dark`f `Bg51mid`b `Fg99light`f");
    bool saw_dark_gray = false;
    bool saw_mid_gray_background = false;
    bool saw_light_gray = false;
    for (const auto& run : inline_grayscale.blocks.front().runs) {
        if (run.text == "dark")
            saw_dark_gray = run.has_foreground && run.foreground == 0x080808;
        else if (run.text == "mid")
            saw_mid_gray_background = run.has_background && run.background == 0x848484;
        else if (run.text == "light")
            saw_light_gray = run.has_foreground && run.foreground == 0xffffff;
    }
    check("inline grayscale foreground and background match Urwid",
          saw_dark_gray && saw_mid_gray_background && saw_light_gray);
    for (const char* invalid : {"g0", "g100", "G50", "gx0", "g0x"}) {
        const auto malformed_grayscale = parser.parse(std::string("#!fg=") + invalid + "\ntext");
        const std::string name = std::string("malformed grayscale is rejected: ") + invalid;
        check(name.c_str(),
              malformed_grayscale.malformed && !malformed_grayscale.has_foreground);
    }

    CompactPage::RunRecord light_background_run{};
    light_background_run.style = CompactPage::HAS_BACKGROUND;
    light_background_run.background = 0xe8b4f0;
    check("focus border contrasts with a light authored inline background",
          resolve_focus_border(color_page, light_background_run, 0x1d1a1e) == 0x000000);
    CompactPage::RunRecord inherited_background_run{};
    check("focus border contrasts with a dark authored page background",
          resolve_focus_border(color_page, inherited_background_run, 0x1d1a1e) == 0xffffff);
    CompactPage::RunRecord saturated_green_run{};
    saturated_green_run.style = CompactPage::HAS_BACKGROUND;
    saturated_green_run.background = 0x00da00;
    check("focus border uses sRGB contrast for saturated green",
          resolve_focus_border(color_page, saturated_green_run, 0x1d1a1e) == 0x000000);
    CompactPage::RunRecord saturated_blue_run{};
    saturated_blue_run.style = CompactPage::HAS_BACKGROUND;
    saturated_blue_run.background = 0x0000ff;
    check("focus border uses sRGB contrast for saturated blue",
          resolve_focus_border(color_page, saturated_blue_run, 0x1d1a1e) == 0xffffff);

    struct FocusFragmentFixture {
        int16_t link_index;
        uint16_t run_index;
        int16_t x;
        int16_t y;
        int16_t width;
        int16_t height;
        uint8_t heading = 0;
        uint8_t heading_level() const { return heading; }
    };
    const std::vector<FocusFragmentFixture> focus_fragments{
        {3, 7, 10, 20, 28, 16},
        {3, 7, 38, 20, 7, 16},
        {3, 7, 45, 20, 35, 16},
        {3, 7, 10, 39, 24, 16},
        {3, 8, 34, 39, 20, 16},
        {4, 9, 60, 39, 30, 16},
    };
    std::vector<UI::LXMF::NomadNet::FocusSpan> focus_spans;
    for_each_focus_span(focus_fragments, 3, [&](const auto& span) {
        focus_spans.push_back(span);
    });
    check("selected multi-word links use one focus outline per visual line and style run",
          focus_spans.size() == 3 &&
              focus_spans[0].x == 10 && focus_spans[0].y == 20 &&
              focus_spans[0].width == 70 && focus_spans[0].height == 16 &&
              focus_spans[0].run_index == 7 &&
              focus_spans[1].x == 10 && focus_spans[1].y == 39 &&
              focus_spans[1].width == 24 && focus_spans[1].height == 16 &&
              focus_spans[1].run_index == 7 &&
              focus_spans[2].x == 34 && focus_spans[2].y == 39 &&
              focus_spans[2].width == 20 && focus_spans[2].height == 16 &&
              focus_spans[2].run_index == 8);

    auto reset_color_doc = parser.parse(
        "#!fg=abc\n`Ff00inline`fpage `[unstyled link`:/page/plain.mu]\n");
    CompactPage reset_color_page;
    bool saw_reset_page_color = false;
    bool saw_unstyled_link_color = false;
    check("foreground reset fixture compacts", reset_color_page.assign(reset_color_doc));
    for (const auto& run : reset_color_page.runs()) {
        const auto view = reset_color_page.text(run);
        const std::string text(view.data(), view.size());
        if (text == "page ") {
            saw_reset_page_color = !(run.style & CompactPage::HAS_FOREGROUND) &&
                                   resolve_foreground(reset_color_page, run, 0xe8b4f0) ==
                                       0xaabbcc;
        } else if (text == "unstyled link") {
            saw_unstyled_link_color = run.link_index >= 0 &&
                                      !(run.style & CompactPage::HAS_FOREGROUND) &&
                                      resolve_foreground(reset_color_page, run, 0xe8b4f0) ==
                                          0xaabbcc;
        }
    }
    check("foreground reset returns to the page foreground", saw_reset_page_color);
    check("unstyled links inherit the page foreground", saw_unstyled_link_color);

    auto default_color_doc = parser.parse("plain\n");
    CompactPage default_color_page;
    check("unstyled Micron keeps the Pyxis fallback foreground",
          default_color_page.assign(default_color_doc) &&
          !default_color_page.runs().empty() &&
          resolve_foreground(default_color_page, default_color_page.runs().front(), 0xe8b4f0) ==
              0xe8b4f0);
    auto background_only_doc = parser.parse("#!bg=abc\nplain\n");
    CompactPage background_only_page;
    check("page background does not replace the fallback foreground",
          background_only_page.assign(background_only_doc) &&
              background_only_page.has_background() &&
              background_only_page.background() == 0xaabbcc &&
              !background_only_page.has_foreground() &&
              resolve_foreground(background_only_page, background_only_page.runs().front(),
                                 0xe8b4f0) == 0xe8b4f0);
    auto foreground_only_doc = parser.parse("#!fg=123\nplain\n");
    CompactPage foreground_only_page;
    check("page foreground does not invent a page background",
          foreground_only_page.assign(foreground_only_doc) &&
              foreground_only_page.has_foreground() &&
              foreground_only_page.foreground() == 0x112233 &&
              !foreground_only_page.has_background());
    compact.clear();
    check("compact page teardown releases every retained record", compact.empty() &&
          compact.arena_bytes() == 0 && compact.blocks().empty() && compact.runs().empty() && compact.links().empty());
    UI::LXMF::NomadNet::Document oversized_compact;
    UI::LXMF::NomadNet::Block oversized_block;
    oversized_block.runs.push_back(UI::LXMF::NomadNet::Run{
        std::string(CompactPage::MAX_ARENA_BYTES + 1, 'x')});
    oversized_compact.blocks.push_back(std::move(oversized_block));
    check("compact page rejects text beyond its independent arena bound",
          !compact.assign(oversized_compact) && compact.empty() && compact.arena_bytes() == 0);
    auto link_fields = parser.parse("`[Search`:/page/search.mu`q=pyxis]\n");
    check("link target excludes request fields", link_fields.links.size() == 1 &&
          link_fields.links[0].target == ":/page/search.mu" && link_fields.links[0].fields == "q=pyxis");
    CompactPage link_fields_page;
    check("compact link navigation retains request fields",
          link_fields_page.assign(link_fields) && link_fields_page.links().size() == 1 &&
          std::string(link_fields_page.target(0).data(), link_fields_page.target(0).size()) ==
              ":/page/search.mu`q=pyxis");
    check("divider parsed", doc.blocks[3].type == BlockType::DIVIDER);
    auto divider_doc = parser.parse("-\n-=\n-\x01\n-long\n-─\n-═\n-║\n-🙂");
    check("canonical divider forms parse as bounded divider metadata",
          divider_doc.blocks.size() == 8 &&
              std::all_of(divider_doc.blocks.begin(), divider_doc.blocks.end(),
                          [](const auto& block) { return block.type == BlockType::DIVIDER; }));
    check("default and control-character dividers use the canonical line glyph",
          divider_doc.blocks[0].divider_codepoint == 0x2500 &&
              divider_doc.blocks[2].divider_codepoint == 0x2500);
    check("exactly two-character divider forms preserve the authored character",
          divider_doc.blocks[1].divider_codepoint == '=' &&
          divider_doc.blocks[4].divider_codepoint == 0x2500 &&
          divider_doc.blocks[5].divider_codepoint == 0x2550 &&
          divider_doc.blocks[6].divider_codepoint == 0x2551 &&
          divider_doc.blocks[7].divider_codepoint == 0x1f642);
    check("long divider lines retain canonical default-glyph behavior",
          divider_doc.blocks[3].divider_codepoint == 0x2500);
    CompactPage divider_page;
    check("compact page preserves divider glyph metadata without text runs",
          divider_page.assign(divider_doc) && divider_page.runs().empty() &&
              divider_page.blocks().size() == divider_doc.blocks.size() &&
              divider_page.blocks()[1].divider_codepoint == '=' &&
              divider_page.blocks()[5].divider_codepoint == 0x2550);
    char divider_utf8[5]{};
    const std::size_t divider_bytes = display_codepoint(0x2550, divider_utf8);
    check("supported authored divider glyph is encoded for bounded drawing",
          divider_bytes == 3 && std::string(divider_utf8, divider_bytes) == "═");
    const std::size_t fallback_bytes = display_codepoint(0x1f642, divider_utf8);
    check("unsupported authored divider glyph uses the display fallback",
          fallback_bytes == 1 && divider_utf8[0] == '?');
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

    check("common NomadNet Unicode punctuation remains intact",
          display_text(u8"release · stable — open → details • done") ==
              u8"release · stable — open → details • done");
    check("Latin accents remain intact", display_text(u8"café Ångström") == u8"café Ångström");
    check("live NomadNet box and block drawing glyphs remain intact",
          display_text(u8"─═║╔╗╚╝█▔■") == u8"─═║╔╗╚╝█▔■");
    check("glyphs outside the bounded browser font degrade without rectangles",
          display_text(u8"status 😀 ok") == "status ? ok");
    check("contiguous cmap boundary codepoints degrade safely",
          display_text(std::string("\x7f") + u8"ƀ↚") == "???");

    std::string huge(DocumentParser::MAX_DOCUMENT_BYTES, 'x');
    huge += "\n#!c=99\n";
    auto huge_doc = parser.parse(huge);
    check("document bytes capped and truncation propagated", huge_doc.truncated &&
                                                             huge_doc.source_bytes == DocumentParser::MAX_DOCUMENT_BYTES);
    check("document byte truncation reports its exact boundary",
          huge_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::DOCUMENT_BYTES) &&
          UI::LXMF::NomadNet::truncation_notice(huge_doc) ==
              "[Page truncated: source exceeds 64 KiB]");
    check("parser never processes metadata beyond retained source", huge_doc.cache_seconds == 0);
    std::string long_line(DocumentParser::MAX_SOURCE_LINE_BYTES + 20, 'q');
    auto line_doc = parser.parse(long_line);
    check("source line capped", line_doc.truncated && !line_doc.blocks.empty() &&
                                line_doc.blocks[0].runs[0].text.size() == DocumentParser::MAX_SOURCE_LINE_BYTES);
    check("source line truncation reports its exact boundary",
          line_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::SOURCE_LINE_BYTES) &&
          UI::LXMF::NomadNet::truncation_notice(line_doc) ==
              "[Page truncated: line exceeds 4096 bytes]");
    auto utf8_boundary = parser.parse(std::string(DocumentParser::MAX_SOURCE_LINE_BYTES - 1, 'x') + "\xe2\x82\xac");
    check("line truncation preserves UTF-8 boundaries", utf8_boundary.truncated && !utf8_boundary.malformed &&
                                                        utf8_boundary.blocks[0].runs[0].text.size() ==
                                                            DocumentParser::MAX_SOURCE_LINE_BYTES - 1);
    auto blank_lines = parser.parse("one\n\nthree\n");
    check("blank lines preserve vertical structure", blank_lines.blocks.size() == 4 &&
                                                      blank_lines.blocks[1].runs[0].text == " ");
    std::string many_blocks;
    for (std::size_t i = 0; i < DocumentParser::MAX_BLOCKS + 1; ++i) many_blocks += "\n";
    auto blocks_doc = parser.parse(many_blocks);
    check("block truncation reports its exact boundary",
          blocks_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::BLOCKS) &&
          UI::LXMF::NomadNet::truncation_notice(blocks_doc) ==
              "[Page truncated: more than 1024 blocks]");
    CompactPage blocks_page;
    check("block-limit page reserves room for its truncation notice",
          blocks_page.assign(blocks_doc) &&
          blocks_page.append_notice(UI::LXMF::NomadNet::truncation_notice(blocks_doc)) &&
          blocks_page.blocks().size() <= CompactPage::MAX_BLOCKS &&
          blocks_page.runs().size() <= CompactPage::MAX_RUNS);
    std::string many_lines;
    for (std::size_t i = 0; i < DocumentParser::MAX_SOURCE_LINES + 50; ++i) many_lines += "#\n";
    auto lines_doc = parser.parse(many_lines);
    check("source line count capped", lines_doc.truncated &&
                                      lines_doc.source_lines <= DocumentParser::MAX_SOURCE_LINES);
    check("source line-count truncation reports its exact boundary",
          lines_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::SOURCE_LINES) &&
          UI::LXMF::NomadNet::truncation_notice(lines_doc) ==
              "[Page truncated: more than 4096 lines]");
    std::string many_runs;
    for (std::size_t i = 0; i < DocumentParser::MAX_RUNS_PER_LINE + 20; ++i) many_runs += "x`!";
    auto runs_doc = parser.parse(many_runs);
    check("runs per line capped", runs_doc.truncated &&
                                    runs_doc.blocks[0].runs.size() == DocumentParser::MAX_RUNS_PER_LINE);
    check("per-line run truncation reports its exact boundary",
          runs_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::RUNS_PER_LINE) &&
          UI::LXMF::NomadNet::truncation_notice(runs_doc) ==
              "[Page truncated: more than 128 styles on one line]");
    std::string global_runs;
    for (std::size_t i = 0; i < DocumentParser::MAX_TOTAL_RUNS; ++i) global_runs += "a`!b`!\n";
    auto global_runs_doc = parser.parse(global_runs);
    std::size_t retained_runs = 0;
    for (const auto& block : global_runs_doc.blocks) retained_runs += block.runs.size();
    check("runs are globally bounded", global_runs_doc.truncated && retained_runs <= DocumentParser::MAX_TOTAL_RUNS);
    check("total-run truncation reports its exact boundary",
          global_runs_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::TOTAL_RUNS) &&
          UI::LXMF::NomadNet::truncation_notice(global_runs_doc) ==
              "[Page truncated: more than 1024 styled runs]");
    CompactPage runs_page;
    check("run-limit page reserves room for its truncation notice",
          runs_page.assign(global_runs_doc) &&
          runs_page.append_notice(UI::LXMF::NomadNet::truncation_notice(global_runs_doc)) &&
          runs_page.blocks().size() <= CompactPage::MAX_BLOCKS &&
          runs_page.runs().size() <= CompactPage::MAX_RUNS);
    std::string exact_blocks_and_runs;
    for (std::size_t i = 0; i < DocumentParser::MAX_TOTAL_RUNS; ++i)
        exact_blocks_and_runs += "x\n";
    exact_blocks_and_runs += "# comment-only tail";
    auto exact_doc = parser.parse(exact_blocks_and_runs);
    check("comment tail does not falsely exceed exact block or run limits",
          !exact_doc.truncated &&
          !exact_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::BLOCKS) &&
          !exact_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::TOTAL_RUNS));
    std::string exact_runs_with_directive;
    for (std::size_t i = 0; i < DocumentParser::MAX_TOTAL_RUNS; ++i)
        exact_runs_with_directive += "x\n";
    exact_runs_with_directive += "#!bg=fff";
    auto directive_tail_doc = parser.parse(exact_runs_with_directive);
    check("page directive after exact run limit is still applied",
          !directive_tail_doc.truncated && directive_tail_doc.has_background &&
          directive_tail_doc.background == 0xffffff);
    std::string exact_runs_with_divider;
    for (std::size_t i = 0; i < DocumentParser::MAX_TOTAL_RUNS / 2; ++i)
        exact_runs_with_divider += "a`!b\n";
    exact_runs_with_divider += "-";
    auto divider_tail_doc = parser.parse(exact_runs_with_divider);
    CompactPage divider_tail_page;
    check("compact page retains divider after exact run limit",
          !divider_tail_doc.truncated && divider_tail_page.assign(divider_tail_doc) &&
          divider_tail_page.blocks().size() == divider_tail_doc.blocks.size() &&
          !divider_tail_page.blocks().empty() &&
          divider_tail_page.blocks().back().type == BlockType::DIVIDER);
    check("run-cap notice eviction preserves trailing divider and record bounds",
          divider_tail_page.append_notice("[Page layout truncated: 1023 fragments]") &&
          divider_tail_page.blocks().size() >= 2 &&
          divider_tail_page.blocks()[divider_tail_page.blocks().size() - 2].type ==
              BlockType::DIVIDER &&
          std::all_of(divider_tail_page.blocks().begin(), divider_tail_page.blocks().end(),
              [&divider_tail_page](const CompactPage::BlockRecord& block) {
                  return block.first_run <= divider_tail_page.runs().size() &&
                      block.run_count <= divider_tail_page.runs().size() - block.first_run;
              }));
    CompactPage exact_page;
    check("full valid compact page can replace tail content with a layout notice",
          exact_page.assign(exact_doc) &&
          exact_page.append_notice("[Page layout truncated: 1023 fragments]") &&
          exact_page.blocks().size() <= CompactPage::MAX_BLOCKS &&
          exact_page.runs().size() <= CompactPage::MAX_RUNS);
    auto truncated_with_divider = parser.parse(
        std::string(DocumentParser::MAX_SOURCE_LINE_BYTES + 1, 'q') + "\n-");
    CompactPage truncated_divider_page;
    check("appending notice preserves retained trailing divider",
          truncated_with_divider.truncated && truncated_divider_page.assign(truncated_with_divider) &&
          truncated_divider_page.append_notice(UI::LXMF::NomadNet::truncation_notice(
              truncated_with_divider)) &&
          truncated_divider_page.blocks().size() >= 2 &&
          truncated_divider_page.blocks()[truncated_divider_page.blocks().size() - 2].type ==
              BlockType::DIVIDER);
    std::string many_links;
    for (std::size_t i = 0; i < DocumentParser::MAX_LINKS + 10; ++i) many_links += "`[x`:/page/x]";
    auto links_doc = parser.parse(many_links);
    check("links are globally bounded", links_doc.truncated && links_doc.links.size() == DocumentParser::MAX_LINKS);
    check("link truncation reports its exact boundary",
          links_doc.has_truncation(UI::LXMF::NomadNet::TruncationReason::LINKS) &&
          UI::LXMF::NomadNet::truncation_notice(links_doc) ==
              "[Page truncated: more than 128 links]");

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
        bool retained_fixture_fields = false;
        CompactPage real_page;
        if (real_page.assign(real)) {
            for (std::size_t i = 0; i < real.links.size(); ++i) {
                if (real.links[i].fields.empty()) continue;
                const auto target = real_page.target(i);
                retained_fixture_fields = std::string(target.data(), target.size()) ==
                    real.links[i].target + "`" + real.links[i].fields;
                break;
            }
        }
        check("authoritative link fields survive compact navigation", retained_fixture_fields);
        check("authoritative fixture remains within bounds", real.blocks.size() <= DocumentParser::MAX_BLOCKS &&
                                                             real.links.size() <= DocumentParser::MAX_LINKS);
    } else {
        check("authoritative Aleph fixture is readable", false);
        check("authoritative fixture yields headings", false);
        check("authoritative fixture yields links", false);
        check("authoritative link fields survive compact navigation", false);
        check("authoritative fixture remains within bounds", false);
    }

    const auto nil = UI::LXMF::NomadNet::no_form_request_data();
    check("no-form request is exact msgpack nil", nil.size() == 1 && nil[0] == 0xc0);
    const auto configured_variables = UI::LXMF::NomadNet::request_data("g=reticulum|r=lxmf");
    const std::vector<uint8_t> expected_variables{
        0x82,
        0xa5, 'v', 'a', 'r', '_', 'g', 0xa9, 'r', 'e', 't', 'i', 'c', 'u', 'l', 'u', 'm',
        0xa5, 'v', 'a', 'r', '_', 'r', 0xa4, 'l', 'x', 'm', 'f'};
    check("configured link variables encode as the NomadNet request-data map",
          configured_variables == expected_variables);
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

    check("exactly full layout is not truncated when no content remains",
          !UI::LXMF::NomadNet::layout_content_truncated(1023, 1023, false));
    check("exactly full layout is truncated when content remains",
          UI::LXMF::NomadNet::layout_content_truncated(1023, 1023, true));
    check("zero-run text block has no layout content",
          !UI::LXMF::NomadNet::block_has_layout_content(BlockType::TEXT, 0));
    check("divider has layout content without a text run",
          UI::LXMF::NomadNet::block_has_layout_content(BlockType::DIVIDER, 0));

    check("virtual viewport maps short pages without scaling",
          VirtualViewport::logical_from_physical(640, 1200, 150, 1200) == 640 &&
              VirtualViewport::physical_from_logical(640, 1200, 150, 1200) == 640);
    check("virtual viewport maps tall pages through bounded LVGL coordinates",
          VirtualViewport::logical_from_physical(14925, 60000, 150, 30000) == 29925 &&
              VirtualViewport::physical_from_logical(29925, 60000, 150, 30000) == 14925);
    check("virtual viewport clamps logical and physical scroll positions",
          VirtualViewport::logical_from_physical(40000, 60000, 150, 30000) == 59850 &&
              VirtualViewport::physical_from_logical(70000, 60000, 150, 30000) == 29850);
    check("virtual viewport keeps bounded overscan around the visible region",
          VirtualViewport::window_top(30000, 150) == 29700 &&
              VirtualViewport::window_bottom(30000, 150, 60000) == 30450);
    check("overscan window clamps at the document boundaries",
          VirtualViewport::window_top(5,75)==0 &&
              VirtualViewport::window_bottom(5,75,60000)==230);

    std::vector<uint8_t> response_bytes(ResponseBuffer::MAX_BYTES,0x5a);
    check("response buffer accepts the exact body limit",
          response.assign(response_bytes.data(),response_bytes.size()) &&
              response.capacity()>=response_bytes.size());
    response.release();
    check("response release returns the normalized PSRAM allocation",
          response.size()==0 && response.capacity()==0);
    check("coalescing accepts adjacent bytes from the same styled run",
          VirtualViewport::can_coalesce(7, 12, 5, 7, 17));
    check("coalescing preserves style and link run boundaries",
          !VirtualViewport::can_coalesce(7, 12, 5, 8, 17) &&
              !VirtualViewport::can_coalesce(7, 12, 5, 7, 18));

    check("compact address preserves short values", compact_address("node:/page/a.mu", 32) == "node:/page/a.mu");
    check("compact address abbreviates destination but preserves path",
          compact_address("a8d24177d946de4f1f0a0fe1af9a1338:/page/index.mu", 30) ==
              "a8d24177...1338 /page/index.mu");
    check("compact address uses compiled-font-safe ASCII",
          compact_address("a8d24177d946de4f1f0a0fe1af9a1338:/page/index.mu", 30).find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/._- ") == std::string::npos);
    check("compact address remains bounded for long paths",
          compact_address("a8d24177d946de4f1f0a0fe1af9a1338:/page/a-very-long-page-name.mu", 24).size() <= 24);

    Library library;
    const std::string node_hash = "a8d24177d946de4f1f0a0fe1af9a1338";
    const std::string page_url = node_hash + ":/page/index.mu";
    check("heard NomadNet node is retained", library.hear_node(node_hash, "Example Node", 100, 2) &&
          library.nodes().size() == 1 && library.nodes()[0].name == "Example Node");
    check("heard node updates in place", library.hear_node(node_hash, "Renamed Node", 200, 1) &&
          library.nodes().size() == 1 && library.nodes()[0].last_heard == 200);
    check("page navigation records bounded recent page", library.record_page(page_url, "Home", 300) &&
          library.pages().size() == 1 && library.pages()[0].title == "Home");
    check("saving page also saves its node", library.set_page_saved(page_url, true) &&
          library.pages()[0].saved && library.nodes()[0].saved);
    Library removal_library;
    removal_library.hear_node(node_hash, "Remove Me", 1, 0);
    removal_library.record_page(page_url, "Remove Me", 1);
    removal_library.set_page_saved(page_url, true);
    check("removing the last saved page removes its derived saved node",
          removal_library.set_page_saved(page_url, false) &&
          !removal_library.page_saved(page_url) && !removal_library.node_saved(node_hash));
    Library atomic_library;
    char bounded_hash[33]{};
    for (std::size_t i = 0; i < Library::MAX_NODES; ++i) {
        std::snprintf(bounded_hash, sizeof(bounded_hash), "%032zx", i + 1);
        atomic_library.set_node_saved(bounded_hash, true);
    }
    for (std::size_t i = 0; i < Library::MAX_PAGES; ++i) {
        const std::string recent = std::string(32, '0').replace(31, 1, "1") +
            ":/page/recent-" + std::to_string(i) + ".mu";
        atomic_library.record_page(recent, "Recent", i + 1);
    }
    const auto before_failed_save = atomic_library.encode();
    const std::string unretainable_page = std::string(32, 'f') + ":/page/new.mu";
    check("failed unrecorded page save is atomic",
          !atomic_library.set_page_saved(unretainable_page, true) &&
          atomic_library.encode() == before_failed_save);
    const auto encoded_library = library.encode();
    Library restored_library;
    check("NomadNet library round trips", restored_library.decode(encoded_library.data(), encoded_library.size()) &&
          restored_library.nodes().size() == 1 && restored_library.pages().size() == 1 &&
          restored_library.pages()[0].saved);
    const uint8_t corrupt_library[] = {'P', 'X', 'N', 'N', '9', '\n'};
    check("corrupt NomadNet library fails closed",
          !restored_library.decode(corrupt_library, sizeof(corrupt_library)) && restored_library.nodes().size() == 1);
    const std::string invalid_utf8_library = "PXNN1\nN\t" + node_hash + "\tff\t1\t0\t0\n";
    Library invalid_utf8_decoded;
    check("persisted invalid UTF-8 metadata is rejected",
          !invalid_utf8_decoded.decode(reinterpret_cast<const uint8_t*>(invalid_utf8_library.data()),
                                       invalid_utf8_library.size()));
    std::string invalid_utf8_url = node_hash + ":/page/";
    invalid_utf8_url.push_back(static_cast<char>(0xff));
    invalid_utf8_url += ".mu";
    Library invalid_url_library;
    check("invalid UTF-8 page URL is rejected at admission",
          !invalid_url_library.record_page(invalid_utf8_url, "Invalid", 1) &&
          !invalid_url_library.set_page_saved(invalid_utf8_url, true));
    std::string encoded_invalid_url;
    static constexpr char HEX[] = "0123456789abcdef";
    for (const unsigned char byte : invalid_utf8_url) {
        encoded_invalid_url.push_back(HEX[byte >> 4]);
        encoded_invalid_url.push_back(HEX[byte & 0x0f]);
    }
    const std::string invalid_utf8_url_record = "PXNN1\nP\t" + encoded_invalid_url + "\t\t1\t0\n";
    check("persisted invalid UTF-8 page URL is rejected",
          !invalid_url_library.decode(reinterpret_cast<const uint8_t*>(invalid_utf8_url_record.data()),
                                      invalid_utf8_url_record.size()));
    Library bounded_library;
    for (std::size_t i = 0; i < Library::MAX_NODES + 8; ++i) {
        char hash[33];
        std::snprintf(hash, sizeof(hash), "%032zx", i + 1);
        bounded_library.hear_node(hash, "node", i, 0);
    }
    check("heard-node library is bounded", bounded_library.nodes().size() == Library::MAX_NODES);
    const std::string pinned_hash = bounded_library.nodes().back().destination_hex;
    bounded_library.set_node_saved(pinned_hash, true);
    for (std::size_t i = Library::MAX_NODES + 8; i < Library::MAX_NODES + 20; ++i) {
        char hash[33];
        std::snprintf(hash, sizeof(hash), "%032zx", i + 1);
        bounded_library.hear_node(hash, "new", i, 0);
    }
    check("saved node survives heard-node eviction", bounded_library.node_saved(pinned_hash));
    Library live_library;
    live_library.hear_node("11111111111111111111111111111111", "Stale", 100, 1);
    live_library.hear_node("22222222222222222222222222222222", "Saved", 100, 1);
    live_library.set_node_saved("22222222222222222222222222222222", true);
    check("unroutable heard node can be pruned without deleting saved nodes",
          live_library.remove_heard_node("11111111111111111111111111111111") &&
          !live_library.remove_heard_node("22222222222222222222222222222222") &&
          live_library.nodes().size() == 1 && live_library.nodes()[0].saved);
    Library sorted_library;
    sorted_library.hear_node("11111111111111111111111111111111", "Older", 100, 1);
    sorted_library.hear_node("22222222222222222222222222222222", "Newer", 200, 1);
    sorted_library.hear_node("33333333333333333333333333333333", "Oldest arrival", 50, 1);
    check("heard nodes are newest first", sorted_library.nodes().front().name == "Newer" &&
          sorted_library.nodes().back().name == "Oldest arrival");
    const uint8_t unsafe_name[] = {' ', 'N', 'o', 'd', 'e', '\n', 'X', 0xff};
    check("announce name is bounded and sanitized",
          sanitize_directory_name(unsafe_name, sizeof(unsafe_name)) == "Node X");
    check("first heading supplies saved-page title",
          page_title("/page/index.mu", {"Example ", "Node"}) == "Example Node");
    check("control characters are rejected in saved URLs",
          !sorted_library.record_page("11111111111111111111111111111111:/page/bad\n.mu", "bad", 1));
    const std::string new_saved_url = "33333333333333333333333333333333:/page/saved.mu";
    sorted_library.record_page("22222222222222222222222222222222:/page/recent.mu", "Recent", 500);
    check("saving an unrecorded page marks the requested URL",
          sorted_library.set_page_saved(new_saved_url, true) && sorted_library.page_saved(new_saved_url) &&
          !sorted_library.page_saved("22222222222222222222222222222222:/page/recent.mu"));
    Library saturated_library;
    for (std::size_t i = 0; i < Library::MAX_NODES; ++i) {
        char hash[33]; std::snprintf(hash, sizeof(hash), "%032zx", i + 1000);
        saturated_library.hear_node(hash, "saved", i, 0);
        saturated_library.set_node_saved(hash, true);
    }
    const std::string orphan_url = "ffffffffffffffffffffffffffffffff:/page/index.mu";
    saturated_library.record_page(orphan_url, "Orphan", 10);
    check("page save fails atomically when its node cannot be retained",
          !saturated_library.set_page_saved(orphan_url, true) && !saturated_library.page_saved(orphan_url));
    const uint8_t overlong_utf8[] = {0xe0, 0x80, 0x80};
    check("overlong UTF-8 is rejected from announce names",
          sanitize_directory_name(overlong_utf8, sizeof(overlong_utf8)).empty());

    ActionMailbox actions;
    check("NomadNet open action is accepted", actions.publish(UserActionKind::OPEN, page_url));
    check("NomadNet save action preserves its own target", actions.publish(UserActionKind::SAVE, new_saved_url));
    UserAction action;
    check("NomadNet actions are FIFO", actions.pop(action) && action.kind == UserActionKind::OPEN &&
          action.target() == page_url);
    check("save target does not drift with current browser URL", actions.pop(action) &&
          action.kind == UserActionKind::SAVE && action.target() == new_saved_url);
    for (std::size_t i = 0; i < ActionMailbox::CAPACITY; ++i)
        check("bounded NomadNet action queue accepts capacity", actions.publish(UserActionKind::OPEN, page_url));
    check("bounded NomadNet action queue rejects ordinary overflow",
          !actions.publish(UserActionKind::OPEN, page_url));
    check("Home supersedes a full pending action queue",
          actions.publish(UserActionKind::HOME, {}) && actions.pop(action) &&
          action.kind == UserActionKind::HOME && !actions.pop(action));
    actions.publish(UserActionKind::SAVE, new_saved_url);
    actions.publish(UserActionKind::OPEN, page_url);
    actions.publish(UserActionKind::BACK, {});
    check("Back preserves an explicit save but cancels pending opens",
          actions.pop(action) && action.kind == UserActionKind::SAVE &&
          actions.pop(action) && action.kind == UserActionKind::BACK && !actions.pop(action));
    actions.publish(UserActionKind::SAVE, new_saved_url);
    actions.publish(UserActionKind::SAVE, new_saved_url);
    actions.publish(UserActionKind::BACK, {});
    check("duplicate queued save toggles are coalesced",
          actions.pop(action) && action.kind == UserActionKind::SAVE &&
          actions.pop(action) && action.kind == UserActionKind::BACK && !actions.pop(action));
    for (std::size_t i = 0; i < ActionMailbox::CAPACITY; ++i)
        actions.publish(UserActionKind::SAVE, new_saved_url + std::to_string(i));
    actions.publish(UserActionKind::BACK, {});
    std::size_t retained_saves = 0;
    while (actions.pop(action) && action.kind == UserActionKind::SAVE) ++retained_saves;
    check("terminal slot preserves every queued explicit save",
          retained_saves == ActionMailbox::CAPACITY && action.kind == UserActionKind::BACK);

    AsyncMailbox progress_mailbox;
    const std::vector<uint8_t> progress_token{1, 2, 3};
    progress_mailbox.expect_request(progress_token);
    check("bounded Resource progress is published",
          progress_mailbox.publish_progress(progress_token, 2048));
    AsyncMailbox::Event progress_event;
    check("bounded Resource progress is observable by the UI owner",
          progress_mailbox.take(progress_event) &&
          progress_event.kind == AsyncMailbox::Kind::PROGRESS &&
          progress_event.transfer_size == 2048);
    check("wire transfer accounting may exceed the response payload limit",
          progress_mailbox.publish_progress(progress_token, AsyncMailbox::MAX_WIRE_BYTES * 2));
    check("large wire transfer accounting remains progress, not oversized payload",
          progress_mailbox.take(progress_event) &&
          progress_event.kind == AsyncMailbox::Kind::PROGRESS &&
          progress_event.transfer_size == AsyncMailbox::MAX_WIRE_BYTES * 2);

    RequestPolicy request_policy;
    check("path discovery deadline does not undercut transport timeout",
          RequestPolicy::PATH_WAIT_MS >= 15000);
    check("first Link timeout performs one bounded fresh-path retry",
          request_policy.on_link_timeout() == RequestPolicy::LinkTimeoutAction::REFRESH_PATH &&
          request_policy.path_refreshes() == 1);
    check("second Link timeout terminates instead of looping forever",
          request_policy.on_link_timeout() == RequestPolicy::LinkTimeoutAction::FAIL &&
          request_policy.path_refreshes() == 1);
    check("fresh-path retry rejects failed stale-route invalidation",
          !RequestPolicy::path_invalidation_succeeded(true) &&
          RequestPolicy::path_invalidation_succeeded(false));
    request_policy.reset();
    check("new page navigation resets the bounded retry budget",
          request_policy.path_refreshes() == 0);

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
