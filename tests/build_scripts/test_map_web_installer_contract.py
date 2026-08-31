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
    assert 'id="map-style"' in source
    assert '<option value="" selected disabled>Select map style…</option>' in source
    for style in ("osm-bright", "dark-matter", "positron", "toner"):
        assert f'value="{style}"' in source
    assert "resolveMuiStyleProfile" in source
    assert "mapSetId: style.id" in source
    assert 'newest pack takes priority' in source
    assert "showDirectoryPicker" in source
    assert "navigator.locks?.request" in source
    assert "cross-tab locking required for safe map installation" in source
    assert "./js/map-installer.js?v=map-pack-v3" in source
    assert "Oxed's Map Tile Downloader" in source
    assert "OpenStreetMap contributors" in source
    assert "suggestMapIdentity" in source
    assert "const suggestion = suggestMapIdentity(file.name);" in source
    assert "mapPackNameInput.value = suggestion.name;" in source
    assert "mapPackIdInput.value = suggestion.packId;" in source
    assert ".map-field select option" in source
    assert "background: var(--card);" in source

    click_start = source.index("mapInstallBtn.addEventListener('click'")
    picker = source.index("const rootDirectory = await window.showDirectoryPicker", click_start)
    click_before_picker = source[click_start:picker]
    assert "resolveMuiStyleProfile" in click_before_picker


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


def test_map_installer_documents_the_single_writer_contract() -> None:
    """B9: both the flasher UI and the docs must state the supported
    concurrency model — no CLI+browser at once, close other flasher tabs,
    wait for verified completion, safe eject, exact retry — and must never
    tell users to clean up marker files."""
    flasher = FLASHER.read_text(encoding="utf-8")
    docs = (ROOT / "docs/offline-map-packs.md").read_text(encoding="utf-8")
    assert 'class="warning map-safety-contract"' in flasher
    # Normalize whitespace so phrase checks survive HTML line-wrapping.
    flasher_norm = " ".join(flasher.split())
    contract_phrases = (
        "close all other Pyxis flasher tabs",
        "command-line installer",
        "mounted card at the same time",
        "completion message",
        "safely eject",
        "retry with the exact same ZIP",
    )
    for phrase in contract_phrases:
        assert phrase in flasher_norm, f"flasher missing: {phrase!r}"
    # The docs carry the same contract, plus the lock-scope limitation.
    docs_norm = " ".join(docs.split())
    assert "do not run the CLI and the browser installer" in docs_norm
    assert "Close all other flasher tabs" in docs_norm
    assert "Wait for the verified completion message" in docs_norm
    assert "same browser profile" in docs_norm
    # No marker-file cleanup instructions anywhere in the map installer docs.
    lower_docs = docs_norm.lower()
    for forbidden in (
        "delete the marker",
        "remove the marker",
        "clean up the marker",
        "delete the active-pack",
        "remove the active-pack",
    ):
        assert forbidden not in lower_docs


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


def test_map_installer_uses_no_invented_filesystem_api_options() -> None:
    # The browser File System Access API has no `exclusive` getFileHandle
    # option and no FileExistsError; the only legitimate `exclusive` is the
    # Web Locks request mode. Production JS and the test mock must stay
    # within that real contract.
    installer = INSTALLER.read_text(encoding="utf-8")
    test_source = NODE_TEST.read_text(encoding="utf-8")
    for source in (installer, test_source):
        assert "FileExistsError" not in source
    # In the installer the only `exclusive` occurrence may be the Web Locks
    # request options object; the test source may only pass it as an ignored
    # unknown option (B1 harness test), never as an exception trigger.
    assert 'exclusive' not in installer.replace("{mode:'exclusive'}", "")
    # Web Locks only serialize same-origin tabs. The deterministic lock name,
    # the fail-closed message for browsers without the primitives, and the
    # scope-limiting comment must all survive refactors, and no comment or
    # error text may claim coordination with the CLI, other origins, or
    # other browser profiles.
    assert "pyxis-map-installer:" in installer
    assert "cross-tab locking required for safe map installation" in installer
    lower = " ".join(installer.lower().replace("//", " ").split())
    assert "not coordinate with the cli" in lower
    assert "another origin" in lower
    assert "another browser profile" in lower
    for overclaim in ("cross-origin", "other profile", "all producers",
                      "both producers", "coordinated by the cli"):
        assert overclaim not in lower
