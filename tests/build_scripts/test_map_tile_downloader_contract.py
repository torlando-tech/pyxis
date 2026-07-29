from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileDownloader.h"
CORE_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileDownloader.cpp"
ADAPTER_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileHttpArduino.h"
ADAPTER_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileHttpArduino.cpp"
MAP_SCREEN = ROOT / "lib/tdeck_ui/UI/LXMF/MapScreen.cpp"
SETTINGS = ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.cpp"


def test_portable_core_is_bounded_and_allocation_free():
    source = CORE_H.read_text() + CORE_CPP.read_text()
    for forbidden in ("std::vector", "std::map", "std::string", "new ", "malloc(", "LittleFS", "SD.begin", "format("):
        assert forbidden not in source
    assert "QUEUE_CAPACITY = 6U" in source
    assert "RESULT_CAPACITY = 6U" in source
    assert "CHUNK_CAPACITY = 4096U" in source
    assert "URL_CAPACITY" in source
    assert "TileKey" in source
    assert "tile.openstreetmap.org" in source
    assert "OpenStreetMap" in source and "attribution" in source.lower()


def test_https_adapter_verifies_peer_with_explicit_ca_and_has_no_credentials():
    source = ADAPTER_H.read_text() + ADAPTER_CPP.read_text()
    assert "WiFiClientSecure" in source
    assert "setCACert" in source
    assert "setInsecure" not in source
    assert "setConnectTimeout" in source
    assert "setTimeout" in source
    for forbidden in ("Authorization", "Cookie", "username", "password", "SD.begin", "format(", "LittleFS"):
        assert forbidden not in source


def test_downloader_is_explicitly_opt_in_and_wired_only_for_visible_misses():
    screen = MAP_SCREEN.read_text()
    settings = SETTINGS.read_text()
    assert "downloadTile(request)" in screen
    assert "downloader_.enqueue(request.key, request.frame_epoch)" in screen
    assert "presenter_.frameEpoch() != request.frame_epoch" in screen
    assert 'KEY_MAP_DOWNLOAD = "map_dl"' in settings
    assert "prefs.getBool(KEY_MAP_DOWNLOAD, false)" in settings
    assert "Download map tiles:" in settings
