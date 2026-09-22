// Host test: image records survive CompactPage::assign() (arena round-trip).
// Verifies the compact model faithfully carries ImageRecord data (alt text,
// url, width/height kind+value, alignment) from the parsed Document into the
// arena-backed CompactPage, and that the image_index on each IMAGE block
// resolves to the correct record.

#include <iostream>
#include <string>

#include "NomadNetCompactPage.h"
#include "NomadNetDocument.h"

using UI::LXMF::NomadNet::Alignment;
using UI::LXMF::NomadNet::BlockType;
using UI::LXMF::NomadNet::CompactPage;
using UI::LXMF::NomadNet::DocumentParser;
using UI::LXMF::NomadNet::ImageDimension;

int main() {
    int failures = 0;
    auto check = [&](const char* name, bool condition) {
        if (!condition) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    DocumentParser parser;

    // 1. Canonical tag survives assign(): fields round-trip byte-for-byte.
    {
        const auto document = parser.parse(
            "`(The RNS logo`w=n`a=c`:/media/demo.webp)");
        CompactPage page;
        check("canonical image compacts", page.assign(document));
        check("exactly one image", page.images().size() == 1);
        const auto& record = page.images()[0];
        const auto alt = page.image_alt(record);
        const auto url = page.image_url(record);
        check("alt round-trips", alt.length == 12 &&
              std::string(alt.value, alt.length) == "The RNS logo");
        check("url round-trips (verbatim with leading ':')",
              url.length == 17 && std::string(url.value, url.length) ==
                  ":/media/demo.webp");
        check("width NATIVE", record.width_kind == ImageDimension::NATIVE);
        check("height NONE (unspecified)", record.height_kind == ImageDimension::NONE);
        check("align CENTER", record.align == Alignment::CENTER);
        // The block that references the image resolves to it.
        bool block_ok = false;
        for (const auto& block : page.blocks()) {
            if (block.type == BlockType::IMAGE) {
                block_ok = block.image_index == 0 &&
                    block.image_index < static_cast<int16_t>(page.images().size());
            }
        }
        check("IMAGE block resolves to image record", block_ok);
    }

    // 2. Multiple images: distinct records, correct per-block resolution.
    {
        const auto document = parser.parse(
            "`(one`w=100`a=l`:/a.webp)\n`(two`w=50%`a=r`:/b.webp)");
        CompactPage page;
        check("multi-image compacts", page.assign(document));
        check("two images", page.images().size() == 2);
        if (page.images().size() == 2) {
            const auto& first = page.images()[0];
            const auto& second = page.images()[1];
            check("first url", std::string(
                      page.image_url(first).value,
                      page.image_url(first).length) == ":/a.webp");
            check("second url", std::string(
                      page.image_url(second).value,
                      page.image_url(second).length) == ":/b.webp");
            check("first width PIXELS 100",
                  first.width_kind == ImageDimension::PIXELS && first.width_value == 100);
            check("second width PERCENT 50",
                  second.width_kind == ImageDimension::PERCENT && second.width_value == 50);
            check("first align LEFT", first.align == Alignment::LEFT);
            check("second align RIGHT", second.align == Alignment::RIGHT);
        }
        // Each IMAGE block points at the correct record.
        std::size_t seen_images = 0;
        bool resolution_ok = true;
        for (const auto& block : page.blocks()) {
            if (block.type != BlockType::IMAGE) continue;
            if (block.image_index != static_cast<int16_t>(seen_images))
                resolution_ok = false;
            ++seen_images;
        }
        check("blocks resolve to distinct records in order", resolution_ok &&
              seen_images == 2);
    }

    // 3. clear() drops the image records; re-assign repopulates them.
    {
        CompactPage page;
        const auto document = parser.parse("`(keep`:/k.webp)");
        check("initial compact", page.assign(document));
        check("one image before clear", page.images().size() == 1);
        page.clear();
        check("zero images after clear", page.images().size() == 0);
        check("re-assign repopulates", page.assign(document) &&
              page.images().size() == 1);
    }

    // 4. Move-assignment preserves image records and keeps every IMAGE
    //    block index valid. Regression: operator=(CompactPage&&) once
    //    swapped every member except _images, so the screen's
    //    `set_page` publish (candidate -> _page move) silently dropped
    //    all image records while blocks kept their image_index.
    {
        const auto document = parser.parse(
            "text\n"
            "`(moved`w=100`h=50`a=r`:/media/m.webp)\n"
            "more");
        CompactPage candidate;
        check("candidate compacts", candidate.assign(document));
        check("candidate has image", candidate.images().size() == 1);

        CompactPage target;
        target = std::move(candidate);
        check("moved page keeps its image", target.images().size() == 1);
        const auto& record = target.images()[0];
        check("moved record fields survive",
              record.width_kind == ImageDimension::PIXELS &&
              record.width_value == 100 &&
              record.height_kind == ImageDimension::PIXELS &&
              record.height_value == 50 &&
              record.align == Alignment::RIGHT &&
              std::string(target.image_url(record).value,
                          target.image_url(record).length) ==
                  ":/media/m.webp");
        check("moved image alt survives",
              std::string(target.image_alt(record).value,
                          target.image_alt(record).length) == "moved");
        bool block_resolves = false;
        for (const auto& block : target.blocks()) {
            if (block.type != BlockType::IMAGE) continue;
            block_resolves = block.image_index == 0 &&
                static_cast<std::size_t>(block.image_index) <
                    target.images().size();
        }
        check("moved IMAGE block resolves to retained record", block_resolves);

        // Move-construction (delegates to move-assignment) must preserve
        // records too.
        CompactPage fresh;
        check("fresh compacts", fresh.assign(document) &&
              fresh.images().size() == 1);
        CompactPage constructed(std::move(fresh));
        check("move-constructed page keeps its image",
              constructed.images().size() == 1);
    }

    if (failures == 0)
        std::cout << "image compact checks passed" << '\n';
    else
        std::cout << failures << " image compact checks FAILED" << '\n';
    return failures == 0 ? 0 : 1;
}
