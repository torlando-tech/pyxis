from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileDownloader.h"
CORE_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileDownloader.cpp"
ADAPTER_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileHttpArduino.h"
ADAPTER_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileHttpArduino.cpp"


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
