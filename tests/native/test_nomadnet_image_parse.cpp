// Host test: micron image-tag parsing in the Pyxis NomadNet DocumentParser.
// Reference semantics are pinned to markqvist/NomadNet e1e8ab8 (v1.4.3):
//   - dispatch on a source line beginning with "`(" (MicronParser.py:550-551),
//     calling parse_image(line[2:]) — i.e. the tag is "`(" alt "` props "` url ")".
//   - parse_image() (MicronParser.py:221): rfind(")"), split on backticks,
//     fields[0]=alt (stripped), fields[-1]=url (stripped), middle fields are
//     key=value props; unknown props ignored; a/l/c align shorthands expand.
//   - len(fields) < 2 or no closing ")" => not an image (reference returns
//     None and the line is rendered as ordinary inline text).
//   - The escape strip (leading backslash) and comment check run BEFORE image
//     dispatch, so an escaped "`(" line is still an image.
//   - Non-rendering clients display the alt-text placeholder.

#include "NomadNetDocument.h"

#include <cstdio>
#include <string>

using namespace UI::LXMF;
using namespace NomadNet;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool image_block_at(const Document& doc, std::size_t index,
                           std::size_t* image_index) {
    if (index >= doc.blocks.size()) return false;
    if (doc.blocks[index].type != BlockType::IMAGE) return false;
    if (doc.blocks[index].image_index < 0) return false;
    if (image_index) *image_index = doc.blocks[index].image_index;
    return *image_index < doc.images.size();
}

static std::string image_alt(const Document& doc, std::size_t image) {
    return doc.images[image].alt;
}

static std::string image_url(const Document& doc, std::size_t image) {
    return doc.images[image].url;
}

static bool any_image(const Document& doc) {
    for (const auto& block : doc.blocks)
        if (block.type == BlockType::IMAGE) return true;
    return false;
}

int main() {
    DocumentParser parser;

    // 1. Canonical upstream example (Guide.py:1409).
    {
        const Document doc = parser.parse(
            "`(The RNS logo`w=n`a=c`:/media/demo.webp)");
        std::size_t image = 0;
        CHECK(doc.blocks.size() == 1);
        CHECK(image_block_at(doc, 0, &image));
        // The url is stored verbatim, including its leading ":" (local-media
        // marker resolved at transport time by url_delegate.resolve_image).
        CHECK(image_url(doc, image) == ":/media/demo.webp");
        CHECK(image_alt(doc, image) == "The RNS logo");
        CHECK(doc.images[image].align == Alignment::CENTER);
        CHECK(doc.images[image].width.kind == ImageDimension::NATIVE);
        // Height was not specified: NONE (the reference leaves it None, i.e.
        // auto), not NATIVE.
        CHECK(doc.images[image].height.kind == ImageDimension::NONE);
    }

    // 2. Minimal form: alt and url only, no props.
    {
        const Document doc = parser.parse("`(/pic`:/pic)");
        std::size_t image = 0;
        CHECK(image_block_at(doc, 0, &image));
        CHECK(image_url(doc, image) == ":/pic");
        CHECK(image_alt(doc, image) == "/pic");
        CHECK(doc.images[image].width.kind == ImageDimension::NONE);
        CHECK(doc.images[image].align == Alignment::LEFT);
    }

    // 3. Alignment shorthands: l, r, c expand to left/right/center. One
    //    image per line (the reference is block-level, one tag per line).
    {
        const Document doc = parser.parse(
            "`(a`a=l`:/a)\n`(b`a=c`:/b)\n`(c`a=r`:/c)");
        std::size_t i0 = 0, i1 = 0, i2 = 0;
        CHECK(image_block_at(doc, 0, &i0));
        CHECK(image_block_at(doc, 1, &i1));
        CHECK(image_block_at(doc, 2, &i2));
        CHECK(doc.images[i0].align == Alignment::LEFT);
        CHECK(doc.images[i1].align == Alignment::CENTER);
        CHECK(doc.images[i2].align == Alignment::RIGHT);
    }

    // 4. Unknown properties are ignored; malformed key=value ignored too.
    {
        const Document doc = parser.parse(
            "`(art`q=99`x`a=c`:/x)");
        std::size_t image = 0;
        CHECK(image_block_at(doc, 0, &image));
        CHECK(image_alt(doc, image) == "art");
        CHECK(image_url(doc, image) == ":/x");
        CHECK(doc.images[image].align == Alignment::CENTER);
        CHECK(doc.images[image].width.kind == ImageDimension::NONE);
    }

    // 5. Width/height property parsing: pixel budget, percent, native.
    {
        const Document doc = parser.parse(
            "`(p`w=100`h=50`:/a.webp)\n`(n`w=n`h=n`:/b.webp)\n`(pc`w=50%`h=25%`:/c.webp)");
        std::size_t i0 = 0, i1 = 0, i2 = 0;
        CHECK(image_block_at(doc, 0, &i0));
        CHECK(image_block_at(doc, 1, &i1));
        CHECK(image_block_at(doc, 2, &i2));
        CHECK(doc.images[i0].width.kind == ImageDimension::PIXELS &&
              doc.images[i0].width.value == 100);
        CHECK(doc.images[i0].height.kind == ImageDimension::PIXELS &&
              doc.images[i0].height.value == 50);
        CHECK(doc.images[i1].width.kind == ImageDimension::NATIVE);
        CHECK(doc.images[i2].width.kind == ImageDimension::PERCENT &&
              doc.images[i2].width.value == 50);
    }

    // 5b. Pixel budget over the cap is clamped, not dropped.
    {
        const Document doc = parser.parse("`(big`w=99999`:/big.webp)`");
        std::size_t image = 0;
        CHECK(image_block_at(doc, 0, &image));
        CHECK(doc.images[image].width.kind == ImageDimension::PIXELS);
        CHECK(doc.images[image].width.value ==
              DocumentParser::MAX_IMAGE_PIXEL_BUDGET);
    }

    // 6. Not an image: no closing paren -> reference returns None and the
    //    line is rendered as ordinary inline text.
    {
        const Document doc = parser.parse("`(/media/none)`");
        CHECK(!any_image(doc));
        bool found_text = false;
        for (const auto& block : doc.blocks)
            for (const auto& run : block.runs)
                if (run.text.find("/media/none") != std::string::npos)
                    found_text = true;
        CHECK(found_text);
    }

    // 7. Not an image: fewer than two backtick fields (no url).
    {
        const Document doc = parser.parse("`(");
        CHECK(!any_image(doc));
    }

    // 8. Image following a section-marker line still parses.
    {
        const Document doc = parser.parse("<\n`(<img`a=c`:/deep)`");
        bool found = false;
        std::size_t image = 0;
        for (std::size_t i = 0; i < doc.blocks.size(); ++i)
            if (image_block_at(doc, i, &image)) { found = true; break; }
        CHECK(found);
        CHECK(image_url(doc, image) == ":/deep");
    }

    // 9. Literal mode: "`(" is ordinary text, never an image.
    {
        const Document doc = parser.parse("`=\n`(/media/lit)\n`=");
        CHECK(!any_image(doc));
    }

    // 10. Escaped line: the reference strips the leading backslash BEFORE
    //     image dispatch, so this is still an image.
    {
        const Document doc = parser.parse("\\`(/media/esc`:/x)`");
        std::size_t image = 0;
        CHECK(image_block_at(doc, 0, &image));
        CHECK(image_url(doc, image) == ":/x");
    }

    // 11. Bounded: more than MAX_IMAGES images truncates, earlier ones
    //     retained.
    {
        std::string source;
        for (int i = 0; i < static_cast<int>(DocumentParser::MAX_IMAGES) + 4; ++i) {
            source += "`(img";
            source += std::to_string(i);
            source += "`:/m";
            source += std::to_string(i);
            source += ".webp)\n";
        }
        Document doc = parser.parse(source);
        CHECK(doc.images.size() == DocumentParser::MAX_IMAGES);
        CHECK(doc.truncated);
        CHECK(doc.has_truncation(TruncationReason::IMAGES));
    }

    // 12. Overlong url is not admitted as an image (bounded field).
    {
        std::string url(400, '/');
        const Document doc = parser.parse("`(/u`/" + url + ")");
        CHECK(!any_image(doc));
        CHECK(doc.truncated);
        CHECK(doc.has_truncation(TruncationReason::IMAGE_URL_BYTES));
    }

    // 13. Overlong alt is retained truncated, not dropped.
    {
        std::string alt(300, 'A');
        const Document doc = parser.parse("`(" + alt + "`:/ok)");
        std::size_t image = 0;
        CHECK(image_block_at(doc, 0, &image));
        CHECK(image_alt(doc, image).size() ==
              DocumentParser::MAX_IMAGE_ALT_BYTES);
    }

    if (failures == 0) std::printf("ALL IMAGE PARSER TESTS PASSED\n");
    return failures == 0 ? 0 : 1;
}
