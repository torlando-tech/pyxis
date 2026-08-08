#include "UI/LXMF/MapStyleSelector.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
int passed = 0;
int failed = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failed; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

Pyxis::MapStyleSummary style(const char* id, const char* label) {
    Pyxis::MapStyleSummary result{};
    std::strncpy(result.id, id, sizeof(result.id) - 1U);
    std::strncpy(result.label, label, sizeof(result.label) - 1U);
    return result;
}

void emptyAndSingleStyleDisableCycling() {
    Pyxis::MapStyleSelector selector;
    Pyxis::MapStyleRequest request{};
    CHECK(selector.state() == Pyxis::MapStyleSelector::State::DISCOVERING);
    CHECK(selector.setCatalog(4U, nullptr, 0U, nullptr));
    CHECK(selector.count() == 0U);
    CHECK(!selector.canCycle());
    CHECK(!selector.requestNext(request));

    const Pyxis::MapStyleSummary only[] = {style("osm-bright", "Bright")};
    CHECK(selector.setCatalog(5U, only, 1U, "osm-bright"));
    CHECK(std::strcmp(selector.activeId(), "osm-bright") == 0);
    CHECK(std::strcmp(selector.activeLabel(), "Bright") == 0);
    CHECK(!selector.canCycle());
    CHECK(!selector.requestNext(request));
}

void cyclesDeterministicallyAndBoundsPendingIntent() {
    Pyxis::MapStyleSelector selector;
    const Pyxis::MapStyleSummary styles[] = {
        style("osm-bright", "Bright"), style("dark-matter", "Dark"),
        style("positron", "Positron"), style("toner", "Toner")};
    CHECK(selector.setCatalog(9U, styles, 4U, "dark-matter"));
    CHECK(selector.canCycle());

    Pyxis::MapStyleRequest request{};
    CHECK(selector.requestNext(request));
    CHECK(request.catalog_generation == 9U);
    CHECK(std::strcmp(request.style_id, "positron") == 0);
    CHECK(request.token != 0U);
    CHECK(selector.state() == Pyxis::MapStyleSelector::State::APPLYING);
    Pyxis::MapStyleRequest duplicate{};
    CHECK(!selector.requestNext(duplicate));

    Pyxis::MapStyleCompletion stale{};
    stale.token = request.token + 1U;
    stale.catalog_generation = request.catalog_generation;
    stale.success = true;
    std::strcpy(stale.style_id, request.style_id);
    CHECK(!selector.complete(stale));
    CHECK(std::strcmp(selector.activeId(), "dark-matter") == 0);

    Pyxis::MapStyleCompletion failure{};
    failure.token = request.token;
    failure.catalog_generation = request.catalog_generation;
    failure.success = false;
    std::strcpy(failure.style_id, request.style_id);
    CHECK(selector.complete(failure));
    CHECK(selector.state() == Pyxis::MapStyleSelector::State::ERROR);
    CHECK(std::strcmp(selector.activeId(), "dark-matter") == 0);

    CHECK(selector.requestNext(request));
    Pyxis::MapStyleCompletion success{};
    success.token = request.token;
    success.catalog_generation = request.catalog_generation;
    success.success = true;
    std::strcpy(success.style_id, request.style_id);
    CHECK(selector.complete(success));
    CHECK(selector.state() == Pyxis::MapStyleSelector::State::READY);
    CHECK(std::strcmp(selector.activeId(), "positron") == 0);
    CHECK(std::strcmp(selector.activeLabel(), "Positron") == 0);
}

void rejectsMalformedCatalogsAndStaleCompletions() {
    Pyxis::MapStyleSelector selector;
    Pyxis::MapStyleSummary duplicate[] = {
        style("osm-bright", "Bright"), style("osm-bright", "Again")};
    CHECK(!selector.setCatalog(1U, duplicate, 2U, "osm-bright"));
    const Pyxis::MapStyleSummary styles[] = {
        style("osm-bright", "Bright"), style("toner", "Toner")};
    CHECK(!selector.setCatalog(0U, styles, 2U, "osm-bright"));
    CHECK(!selector.setCatalog(2U, styles, 2U, "unknown"));
    CHECK(selector.setCatalog(3U, styles, 2U, "osm-bright"));
    Pyxis::MapStyleRequest request{};
    CHECK(selector.requestNext(request));
    CHECK(selector.setCatalog(4U, styles, 2U, "osm-bright"));
    Pyxis::MapStyleCompletion old{};
    old.token = request.token;
    old.catalog_generation = request.catalog_generation;
    old.success = true;
    std::strcpy(old.style_id, request.style_id);
    CHECK(!selector.complete(old));
    CHECK(std::strcmp(selector.activeId(), "osm-bright") == 0);
}

void permitsSoleInstalledStyleWhenNoRecognizedStyleIsActive() {
    Pyxis::MapStyleSelector selector;
    const Pyxis::MapStyleSummary only[] = {style("positron", "Positron")};
    CHECK(selector.setCatalog(9U, only, 1U, nullptr));
    CHECK(selector.canCycle());
    CHECK(std::strcmp(selector.activeId(), "") == 0);
    CHECK(std::strcmp(selector.activeLabel(), "Style") == 0);
    Pyxis::MapStyleRequest request{};
    CHECK(selector.requestNext(request));
    CHECK(std::strcmp(request.style_id, "positron") == 0);
}
}

int main() {
    emptyAndSingleStyleDisableCycling();
    cyclesDeterministicallyAndBoundsPendingIntent();
    rejectsMalformedCatalogsAndStaleCompletions();
    permitsSoleInstalledStyleWhenNoRecognizedStyleIsActive();
    std::cout << "map style selector: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
