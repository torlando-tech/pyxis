from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADAPTER = ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationPersistenceLittleFS.cpp"
HEADER = ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationPersistenceLittleFS.h"
MAIN = ROOT / "src" / "main.cpp"


def test_location_persistence_never_mounts_or_formats_littlefs():
    source = ADAPTER.read_text() + HEADER.read_text()
    assert ".format(" not in source
    assert "LittleFS.format" not in source
    assert "LittleFS.begin" not in source
    assert "already-mounted LittleFS" in source


def test_location_persistence_uses_isolated_temp_live_backup_paths():
    source = ADAPTER.read_text() + HEADER.read_text()
    assert '"/littlefs/location_state.bin"' in source
    assert '"/littlefs/location_state.tmp"' in source
    assert '"/littlefs/location_state.bak"' in source


def test_location_persistence_vfs_reports_errors_and_syncs_durably():
    source = ADAPTER.read_text()
    assert "::stat(" in source
    assert "errno == ENOENT" in source
    assert "std::fflush(" in source
    assert "::fsync(" in source
    assert "std::fclose(" in source
    assert 'LittleFSFileSystem fs("/littlefs")' in MAIN.read_text()
