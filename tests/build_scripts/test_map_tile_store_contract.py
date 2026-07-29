from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileStore.h"
CORE_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileStore.cpp"
SD_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileStoreSD.h"
SD_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileStoreSD.cpp"
SD_ACCESS_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/SDAccess.cpp"


def test_sd_adapter_uses_existing_mount_without_begin_or_format():
    source = SD_H.read_text() + SD_CPP.read_text()
    assert "SDAccess::is_ready" in source
    assert "SDAccess::acquire_bus" in source
    assert "SDAccess::release_bus" in source
    assert "SD.begin" not in source
    assert ".begin(" not in source
    assert "format(" not in source
    assert "LittleFS" not in source
    assert "already-mounted SD" in source


def test_portable_core_has_no_dynamic_standard_containers_or_paths_from_callers():
    source = CORE_H.read_text() + CORE_CPP.read_text()
    header = CORE_H.read_text()
    for forbidden in ("std::vector", "std::map", "std::string", "LittleFS"):
        assert forbidden not in source
    assert '"/pyxis-map/tiles/' in source
    assert "TileKey" in source
    assert "beginGet(const char*" not in header
    assert "beginPut(const char*" not in header


def test_adapter_releases_shared_bus_for_each_chunk():
    source = SD_CPP.read_text()
    assert "readChunk" in source and "writeChunk" in source
    assert source.count("SDAccess::release_bus()") >= 8


def test_global_sd_mount_never_formats_unrecognized_media():
    source = SD_ACCESS_CPP.read_text()
    assert "format_if_empty=true" not in source
    assert 'SD.begin(SDCard::CS, SPI, SD_SPI_FREQ, "/sd", 5, false)' in source
