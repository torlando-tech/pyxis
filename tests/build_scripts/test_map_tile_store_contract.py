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


def test_sd_adapter_latches_unhealthy_when_open_handles_cannot_close():
    header = SD_H.read_text()
    source = SD_CPP.read_text()
    assert "bool healthy_;" in header
    assert source.count("healthy_ = false") >= 3
    assert "if (!healthy_) return false" in source


def test_sd_adapter_distinguishes_missing_files_from_open_failures():
    source = SD_CPP.read_text()
    body = source[source.index("TileStoreResult MapTileStoreSD::beginRead"):
                  source.index("TileStoreResult MapTileStoreSD::readChunk")]
    assert "statMountedPathLocked(name, info)" in body
    assert body.index("statMountedPathLocked(name, info)") < body.index("SD.open(name, FILE_READ)")
    assert "if (!stream_) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }" in body
    stat_body = source[source.index("TileStoreResult MapTileStoreSD::stat"):
                       source.index("TileStoreResult MapTileStoreSD::beginList")]
    assert "SD.open" not in stat_body
    assert "statMountedPathLocked(name, info)" in stat_body
    list_body = source[source.index("TileStoreResult MapTileStoreSD::beginList"):
                       source.index("TileStoreResult MapTileStoreSD::nextList")]
    assert "present == TileStoreResult::MISS" in list_body
    assert "if (!list_root_)" in list_body and "TileStoreResult::IO_ERROR" in list_body


def test_sd_adapter_checks_sync_and_close_before_acknowledging_write():
    source = SD_CPP.read_text()
    body = source[source.index("TileStoreResult MapTileStoreSD::commitWrite"):
                  source.index("void MapTileStoreSD::abortWrite")]
    assert "::fsync(write_fd_) == 0" in body
    assert "::close(write_fd_) == 0" in body
    assert "synced && closed" in body
    assert "stream_.flush()" not in body


def test_sd_abort_lock_timeout_defers_descriptor_close_without_leaking_it():
    source = SD_CPP.read_text()
    abort = source[source.index("void MapTileStoreSD::abortWrite"):
                   source.index("TileStoreResult MapTileStoreSD::remove")]
    service = source[source.index("TileStoreResult MapTileStoreSD::servicePendingAbortLocked"):
                     source.index("bool MapTileStoreSD::isAvailable")]
    begin_write = source[source.index("TileStoreResult MapTileStoreSD::beginWrite"):
                         source.index("TileStoreResult MapTileStoreSD::writeChunk")]
    assert "else {\n        abort_pending_ = true;\n    }" in abort
    assert "::close(write_fd_)" in service
    assert "servicePendingAbortLocked()" in begin_write
    for method in ("isAvailable", "beginRead", "readChunk", "endRead", "beginWrite",
                   "writeChunk", "commitWrite", "remove", "rename", "stat",
                   "beginList", "nextList", "endList"):
        start = source.index(f"MapTileStoreSD::{method}")
        next_method = source.find("MapTileStoreSD::", start + len(f"MapTileStoreSD::{method}"))
        body = source[start:] if next_method < 0 else source[start:next_method]
        assert "servicePendingAbortLocked()" in body, method
