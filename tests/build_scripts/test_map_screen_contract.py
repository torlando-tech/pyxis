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
    assert "static_assert(MapTileStore::HARD_MAX_ENTRIES == 128" in screen_cpp
    assert "© OpenStreetMap contributors" in screen_cpp
    assert "worker_exited_" in screen_h
    assert "if (!center_initialized_ && request.has_local_location" in screen_cpp


def test_worker_predecodes_and_render_path_has_no_io():
    source = text(UI / "MapScreen.cpp")
    assert "lodepng_decode(" in source
    assert "lodepng_inspect" in source
    assert "max_output_size" in source
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


def test_sd_adapter_never_mounts_or_formats():
    source = text(HW / "MapTileStoreSD.cpp")
    for forbidden in ("SD.begin", "SD.format", "LittleFS"):
        assert forbidden not in source


def test_ui_manager_services_before_lock_and_hides_map_everywhere():
    source = text(UI / "UIManager.cpp")
    update = function_body(source, "void UIManager::update()")
    assert update.index("_map_screen->serviceIo()") < update.index("LVGL_LOCK();")
    assert update.index("_map_screen->updateModel") < update.index("LVGL_LOCK();")
    assert update.index("_map_screen->applyOneCompletion()") > update.index("LVGL_LOCK();")
    assert "SCREEN_MAP" in text(UI / "UIManager.h")
    assert "void UIManager::show_map()" in source
    navigation = [
        "show_conversation_list", "show_chat", "show_compose", "show_announces",
        "show_status", "show_settings", "show_propagation_nodes",
    ]
    for name in navigation:
        body = function_body(source, f"void UIManager::{name}(")
        assert "_map_screen->hide()" in body
    map_body = function_body(source, "void UIManager::show_map()")
    for screen in ("_conversation_list_screen", "_chat_screen", "_compose_screen",
                   "_announce_list_screen", "_status_screen", "_qr_screen",
                   "_settings_screen", "_propagation_nodes_screen", "_call_screen"):
        assert screen in map_body


def test_conversation_navigation_has_five_buttons():
    header = text(UI / "ConversationListScreen.h")
    source = text(UI / "ConversationListScreen.cpp")
    assert "using MapCallback = std::function<void()>;" in header
    assert "set_map_callback" in header and "_map_callback" in header
    assert "LV_SYMBOL_GPS" in function_body(source, "void ConversationListScreen::create_bottom_nav()")
    assert "i < 5" in source
    assert "52, 28" in source
