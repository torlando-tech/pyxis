"""Static boundedness and lock-boundary contracts for the offline map UI."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "lib/tdeck_ui/UI/LXMF"
HW = ROOT / "lib/tdeck_ui/Hardware/TDeck"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated function {signature}")


def test_fixed_pool_and_cache_contracts():
    presenter = text(UI / "MapScreenPresenter.h")
    screen_h = text(UI / "MapScreen.h")
    screen_cpp = text(UI / "MapScreen.cpp")
    lv_conf = text(ROOT / "lib/lv_conf.h")
    assert "TILE_SLOT_COUNT = 6" in presenter
    assert "TILE_REQUEST_CAPACITY = 6" in presenter
    assert "TILE_COMPLETION_CAPACITY = 6" in presenter
    assert "MARKER_COUNT = 33" in screen_h
    assert "approximation_halos_[MARKER_COUNT]" in screen_h
    assert "VIEWPORT_WIDTH = 320" in presenter
    assert "VIEWPORT_HEIGHT = 208" in presenter
    assert "MAX_COMPRESSED_TILE_BYTES = 384U * 1024U" in screen_h
    assert re.search(r"#define\s+LV_IMG_CACHE_DEF_SIZE\s+0\b", lv_conf)
    assert "std::vector" not in presenter + screen_h + screen_cpp
    assert "std::map" not in presenter + screen_h + screen_cpp
    assert '#include "Hardware/TDeck/MapTileStore.h"' not in screen_h
    assert "Hardware::TDeck::MapTileStore store_;" not in screen_h
    assert "TileStoreConfig" not in screen_h + screen_cpp
    assert "pack_.metadata().attribution" in screen_cpp
    assert '"No active map pack"' in screen_cpp
    assert "presenter_.visibleTileStatus" in screen_cpp
    assert "setStatusFor(completion.result)" not in screen_cpp
    assert 'lv_label_set_text(status_label_, "Loading tiles...")' in screen_cpp
    assert screen_cpp.count("refreshVisibleStatus();") >= 2
    assert "© OpenStreetMap contributors" not in screen_cpp
    assert "worker_exited_" in screen_h
    assert "if (!center_initialized_ && request.has_local_location" in screen_cpp


def test_worker_predecodes_and_render_path_has_no_io():
    source = text(UI / "MapScreen.cpp")
    header = text(UI / "MapScreen.h")
    assert "lodepng_decode(" in source
    assert "lodepng_inspect" in source
    assert "max_output_size" in source
    assert "store_.removeTile(request.key)" not in source
    assert "decode_failed_keys_" not in header
    assert "decodeFailedFor" not in source
    assert "markDecodeFailed" not in source
    assert "beginGet" in source and "readGetChunk" in source
    assert 'lv_img_set_src(tile_images_[index], &tile_descriptors_[index])' in source
    assert 'lv_img_set_src(tile_images_[index], "' not in source
    for signature in ("void MapScreen::applyFrame()",
                      "bool MapScreen::applyOneCompletion()"):
        body = function_body(source, signature)
        for forbidden in ("SDAccess", "SD.", "beginGet", "readGetChunk",
                          "lodepng", "new ", "delete ", "lv_obj_create"):
            assert forbidden not in body
    assert "MAX_COMPLETIONS_PER_TICK = 1" in text(UI / "MapScreen.h")


def test_worker_stages_pixels_until_current_completion_is_admitted():
    source = text(UI / "MapScreen.cpp")
    header = text(UI / "MapScreen.h")
    read_tile = function_body(source, "Pyxis::MapTileLoadResult MapScreen::readTile(")
    compressed = function_body(
        source, "Pyxis::MapTileLoadResult MapScreen::readCompressedTile(")
    worker = function_body(source, "void MapScreen::workerLoop()")

    assert "lv_color_t* worker_pixels_;" in header
    assert "worker_pixels_" in read_tile
    assert "worker_pixels_" in compressed
    assert "tile_pixels_" not in read_tile
    assert "tile_pixels_" not in compressed
    assert "presenter_.publishCompletion(completion)" in worker
    assert "std::memcpy(tile_pixels_[completion.slot_index], worker_pixels_" in worker
    assert (worker.index("presenter_.publishCompletion(completion)") <
            worker.index("std::memcpy(tile_pixels_[completion.slot_index], worker_pixels_"))


def test_unchanged_model_does_not_starve_released_tile_requests():
    source = text(UI / "MapScreen.cpp")
    presenter = text(UI / "MapScreenPresenter.h")
    update = function_body(source, "void MapScreen::updateModel(")
    assert "frameBuiltForCurrentEpoch" in presenter
    guard = update.index("if (!presenter_.frameBuiltForCurrentEpoch())")
    revoke = update.index("requests_released_ = false")
    build = update.index("presenter_.buildFrame(request)")
    assert guard < revoke < build


def test_selected_pack_is_the_only_sd_tile_source():
    source = text(UI / "MapScreen.cpp")
    header = text(UI / "MapScreen.h")
    assert '#include "Hardware/TDeck/MapTilePack.h"' in header
    assert "Hardware::TDeck::MapTilePack pack_;" in header
    constructor = source[source.index("MapScreen::MapScreen"):
                         source.index("MapScreen::~MapScreen")]
    assert "pack_(storage_)" in constructor

    worker = function_body(source, "void MapScreen::workerLoop()")
    assert "pack_.initialize()" in worker
    assert "store_.initialize()" not in worker

    read_tile = function_body(source, "Pyxis::MapTileLoadResult MapScreen::readTile(")
    assert (read_tile.index("decoded_tile_cache_.get") <
            read_tile.index("readCompressedTile(request)"))
    assert "LIVE_STORE" not in read_tile

    pack_read = function_body(
        source, "Pyxis::MapTileLoadResult MapScreen::readCompressedTile(")
    assert "MapTileStreamReader::readExact" in pack_read
    assert "PackReadStream pack_stream(pack_)" in pack_read
    assert "LiveReadStream" not in source
    pack_adapter = source[source.index("class PackReadStream"):
                          source.index("class AtomicStopSource")]
    assert "pack_.beginGet" in pack_adapter
    assert "pack_.readGetChunk" in pack_adapter
    assert "pack_.endGet" in pack_adapter
    assert "remove" not in pack_adapter
    assert '#include "Hardware/TDeck/MapTileStore.h"' not in header
    assert "Hardware::TDeck::MapTileStore store_;" not in header
    assert "MapTileLookupPolicy" not in source + header
    assert not (UI / "MapTileLookupPolicy.h").exists()
    assert not (UI / "MapTileLookupPolicy.cpp").exists()
    assert "store_initialized_" not in source + header
    assert "store_config_" not in source + header
    assert "LIVE_STORE" not in source + header
    assert "/pyxis-map/tiles" not in source + header

    # Covered-missing, uncovered, and corrupt immutable-pack tiles remain typed
    # pack results. Production never falls through to an untyped style-less cache.
    assert "MapTilePackResult::UNCOVERED" in pack_adapter
    assert "MapTilePackResult::TILE_MISSING" in pack_adapter
    load_tile = function_body(source, "Pyxis::MapTileLoadResult MapScreen::loadTile(")
    assert "return readTile(request);" in load_tile

    # Every activation publishes a durable pack refresh edge. The worker owns
    # reinitialization and clears decoded tiles only when selection identity
    # actually changes; failed replacement remains transactional in MapTilePack.
    show = function_body(source, "void MapScreen::show()")
    assert "pack_refresh_epoch_.fetch_add" in show
    assert "std::atomic<std::uint32_t> pack_refresh_epoch_;" in header
    assert "pack_refresh_epoch != handled_pack_refresh_epoch" in worker
    initial_epoch = worker.index("handled_pack_refresh_epoch")
    initial_pack_init = worker.index("pack_.initialize()")
    assert initial_epoch < initial_pack_init
    assert worker.index("pack_.initialize()", worker.index("pack_refresh_epoch !=")) < worker.index("presenter_.takeRequest")
    assert "decoded_tile_cache_.clear()" in worker
    assert "std::strcmp(previous_pack_id, pack_.metadata().pack_id)" in worker

    library = text(ROOT / "lib/tdeck_ui/library.json")
    assert '"+<Hardware/TDeck/*.cpp>"' in library
    assert '"-<Hardware/TDeck/MapTileDownloader.cpp>"' in library
    assert '"-<Hardware/TDeck/MapTileHttpArduino.cpp>"' in library


def test_sd_adapter_never_mounts_or_formats():
    source = text(HW / "MapTileStoreSD.cpp")
    for forbidden in ("SD.begin", "SD.format", "LittleFS"):
        assert forbidden not in source


def test_style_switch_is_bounded_and_worker_owned():
    source = text(UI / "MapScreen.cpp")
    header = text(UI / "MapScreen.h")
    assert '#include "MapStyleSelector.h"' in header
    assert '#include "Hardware/TDeck/MapStyleCatalog.h"' in header
    assert "Hardware::TDeck::MapStyleCatalog style_catalog_;" in header
    assert "Pyxis::MapStyleSelector style_selector_;" in header
    assert "lv_obj_t* style_button_;" in header
    assert "lv_obj_t* style_label_;" in header
    assert "Pyxis::MapStyleRequest style_request_;" in header
    assert "bool style_request_pending_;" in header
    assert "std::uint32_t style_request_lifecycle_epoch_;" in header
    assert "std::uint32_t style_lifecycle_epoch_;" in header
    assert "bool style_lifecycle_exhausted_;" in header

    callback = function_body(source, "void MapScreen::onStyle(lv_event_t* event)")
    assert "style_selector_.requestNext" in callback
    assert "style_request_pending_ = true" in callback
    assert "style_request_lifecycle_epoch_" in callback
    assert "screen->style_lifecycle_epoch_;" in callback
    assert "style_catalog_.activate" not in callback
    assert "pack_.initialize" not in callback

    worker = function_body(source, "void MapScreen::workerLoop()")
    assert "style_catalog_.discover()" in worker
    assert "style_catalog_.activate" in worker
    assert "style_selector_.activationOwnedBy(style_request_.token)" in worker
    assert "style_selector_.releaseActivation(style_request.token)" in worker
    assert "pack_.initialize()" in worker
    assert "presenter_.invalidateTiles()" in worker
    activation = worker.index("style_catalog_.activate")
    admission_lock = worker.rindex("lockState(portMAX_DELAY)", 0, activation)
    admission_guard = worker.rindex("activation_admitted =", 0, activation)
    admission_unlock = worker.rindex("unlockState();", 0, activation)
    assert admission_lock < admission_guard < admission_unlock < activation
    assert "style_request_lifecycle_epoch == style_lifecycle_epoch_" in worker[admission_guard:activation]
    assert "style_selector_.activationOwnedBy(style_request.token)" in worker[admission_guard:activation]
    assert "if (!activation_admitted) continue;" in worker[:activation]
    assert "&MapScreen::beginStyleActivationCommit" in worker
    commit_guard = function_body(source, "bool MapScreen::beginStyleActivationCommit(void* raw_context)")
    assert "lockState(portMAX_DELAY)" in commit_guard
    assert "screen_visible_.load" in commit_guard
    assert "lifecycle_epoch == screen->style_lifecycle_epoch_" in commit_guard
    assert "activationOwnedBy(context->token)" in commit_guard
    assert commit_guard.index("screen->unlockState()") < commit_guard.index("return admitted")
    committed_reload = worker.index("pack_.initialize()", activation)
    cache_invalidation = worker.index("decoded_tile_cache_.clear()", committed_reload)
    selector_completion = worker.index("style_selector_.complete", committed_reload)
    tile_invalidation = worker.index("presenter_.invalidateTiles()", committed_reload)
    committed_catalog = worker.index("publishStyleCatalog(style_request.token", tile_invalidation)
    rediscovery = worker.index("style_catalog_.discover()", committed_reload)
    assert activation < committed_reload < cache_invalidation
    assert cache_invalidation < selector_completion < tile_invalidation < committed_catalog < rediscovery
    assert "publishStyleCatalog(style_request.token" in worker
    assert "style_request_lifecycle_epoch, !success" in worker
    assert "retain_style_error" not in worker
    release = worker.index("style_selector_.releaseActivation(style_request.token)")
    assert rediscovery < release
    assert "success = discovery" not in worker
    assert worker.index("style_catalog_.activate") < worker.index("presenter_.takeRequest")

    for signature in ("void MapScreen::applyFrame()",
                      "bool MapScreen::applyOneCompletion()"):
        body = function_body(source, signature)
        assert "style_catalog_." not in body
        assert "pack_.initialize" not in body
    assert "lv_group_add_obj(group, style_button_)" in source
    assert "lv_group_remove_obj(style_button_)" in source
    publisher = function_body(source, "void MapScreen::publishStyleCatalog(")
    assert "activation_failed &&" in publisher
    assert "screen_visible_.load" in publisher
    assert "!style_lifecycle_exhausted_" in publisher
    assert "request_lifecycle_epoch == style_lifecycle_epoch_" in publisher
    assert publisher.index("lockState(portMAX_DELAY)") < publisher.index("activation_failed &&")
    hide = function_body(source, "void MapScreen::hide()")
    assert "lockState(portMAX_DELAY)" in hide
    assert hide.index("lockState(portMAX_DELAY)") < hide.index("screen_visible_.store(false")
    assert "style_lifecycle_exhausted_" in hide
    assert "UINT32_MAX" in hide
    assert "style_selector_.cancelPending(style_request_.token)" in hide
    assert "style_selector_.clearError()" in hide


def test_marker_labels_contrast_active_basemap_style():
    source = text(UI / "MapScreen.cpp")
    # The allowlist is light basemaps except one dark style, so pin labels are
    # black by default and white only over the dark basemap.
    assert "bool isDarkBasemap(const char* style_id)" in source
    dark = function_body(source, "bool isDarkBasemap(const char* style_id)")
    assert "std::strcmp(style_id, \"dark-matter\") == 0" in dark

    frame = function_body(source, "void MapScreen::applyFrame()")
    assert "isDarkBasemap(style_selector_.activeId())" in frame
    # The ternary text encodes the contract: dark basemap -> white,
    # otherwise -> black (so black is the default for light basemaps).
    assert "lv_color_white() : lv_color_black()" in frame
    assert "lv_obj_set_style_text_color(marker_labels_[index], marker_label_color, 0)" in frame
    # The color is derived once per frame and applied inside the marker loop.
    derive = frame.index("marker_label_color")
    apply = frame.index("lv_obj_set_style_text_color(marker_labels_[index], marker_label_color")
    assert derive < apply


def test_ui_manager_services_before_lock_and_hides_map_everywhere():
    source = text(UI / "UIManager.cpp")
    update = function_body(source, "void UIManager::update()")
    assert update.index("_map_screen->serviceIo()") < update.index("LVGL_LOCK();")
    assert update.index("_map_screen->updateModel") < update.index("LVGL_LOCK();")
    assert update.index("_map_screen->applyOneCompletion()") > update.index("LVGL_LOCK();")
    assert "MAP" in text(UI / "NavigationStack.h")
    assert "void UIManager::show_map()" in source
    hide_all = function_body(source, "void UIManager::hide_all_screens()")
    assert "_map_screen->hide()" in hide_all
    render = function_body(source, "void UIManager::render_route(")
    assert "hide_all_screens();" in render
    assert "case Route::MAP:" in render
    assert "_map_screen->show();" in render
    map_body = function_body(source, "void UIManager::show_map()")
    assert "navigate(Route::MAP);" in map_body


def test_launcher_exposes_map_as_a_fifth_application():
    header = text(UI / "HomeScreen.h")
    source = text(UI / "HomeScreen.cpp")
    manager = text(UI / "UIManager.cpp")
    assert "APP_COUNT = 5" in header
    assert "set_map_callback" in header and "_map" in header
    assert '"Maps"' in source
    assert '"Offline SD packs"' in source
    assert "LV_SYMBOL_GPS" in source
    assert "LV_ALIGN_LEFT_MID, 0, 0" in source
    assert "LV_ALIGN_TOP_LEFT, 24, 3" in source
    assert "LV_ALIGN_BOTTOM_LEFT, 24, -3" in source
    assert "LV_ALIGN_TOP_LEFT, 0, 44" not in source
    assert "self->_map" in function_body(source, "void HomeScreen::clicked(")
    assert "_home_screen->set_map_callback" in manager
    assert "_conversation_list_screen->set_map_callback" not in manager
