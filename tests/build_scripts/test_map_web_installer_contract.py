"""Contracts for local-only MUI ZIP installation onto a selected SD card."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
FLASHER = ROOT / "docs/flasher/index.html"
INSTALLER = ROOT / "docs/flasher/js/map-installer.js"
NODE_TEST = ROOT / "tests/web/test_map_installer.mjs"


def test_map_installer_ui_is_local_file_to_sd_and_offline_only() -> None:
    source = FLASHER.read_text(encoding="utf-8")
    assert 'id="map-archive"' in source
    assert 'accept=".zip,application/zip"' in source
    assert 'id="map-pack-name"' in source
    assert 'id="map-install-btn"' in source
    assert 'Install and Enable' in source
    assert "mapSetId: 'osm-bright'" in source
    assert 'newest pack takes priority' in source
    assert "showDirectoryPicker" in source
    assert "navigator.locks?.request" in source
    assert "cross-tab locking required for safe map installation" in source
    assert "./js/map-installer.js" in source
    assert "Coalition MUI OSM Bright user download" in source
    assert "Map data (c) OpenStreetMap contributors" in source


def test_map_installer_explains_the_complete_sd_card_workflow() -> None:
    source = FLASHER.read_text(encoding="utf-8")
    assert '<ol class="map-install-steps">' in source
    assert (
        'href="https://download.tiles.coalition.space/" '
        'target="_blank" rel="noopener noreferrer"'
    ) in source
    expected_steps = (
        "Download a compatible MUI map ZIP",
        "Turn the T-Deck off",
        "Choose the downloaded ZIP",
        "Click <strong>Install and Enable</strong>",
        "Safely eject the SD card",
    )
    positions = [source.index(step) for step in expected_steps]
    assert positions == sorted(positions)


def test_map_installer_has_no_tile_network_fetch_path() -> None:
    source = INSTALLER.read_text(encoding="utf-8").lower()
    for forbidden in ("fetch(", "xmlhttprequest", "websocket", "http://", "https://"):
        assert forbidden not in source


def test_map_installer_javascript_behavior() -> None:
    result = subprocess.run(
        ["node", "--test", str(NODE_TEST)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
