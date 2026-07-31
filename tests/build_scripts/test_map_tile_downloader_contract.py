from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CORE_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileDownloader.h"
CORE_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileDownloader.cpp"
ADAPTER_H = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileHttpArduino.h"
ADAPTER_CPP = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileHttpArduino.cpp"
MAP_CA = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileCa.h"
MAP_SCREEN = ROOT / "lib/tdeck_ui/UI/LXMF/MapScreen.cpp"
SETTINGS = ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.cpp"
SETTINGS_H = ROOT / "lib/tdeck_ui/UI/LXMF/SettingsScreen.h"
UI_MANAGER = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
MAIN = ROOT / "src/main.cpp"


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
    assert "setHandshakeTimeout" in source
    assert "setReuse(true)" in source
    assert "useHTTP10(true)" not in source
    assert "disconnectIdle" in source
    reset = ADAPTER_CPP.read_text().split("void MapTileHttpArduino::reset()", 1)[1].split("}\n", 1)[0]
    assert "http_.setReuse(false)" in reset
    assert "http_.end()" in reset
    assert "http_.detachClient()" in reset
    assert "client_.markStopped()" in reset
    assert "MapTileHttpArduino::~MapTileHttpArduino() { reset(); }" in source
    # The pinned WiFiClientSecure zeroes its socket context after an internal
    # failure stop; an explicit second stop can therefore close descriptor 0.
    assert "client_.stop()" not in reset
    for forbidden in ("Authorization", "Cookie", "username", "password", "SD.begin", "format(", "LittleFS"):
        assert forbidden not in source


def test_default_endpoint_uses_current_chain_with_known_fallback_available():
    ca = MAP_CA.read_text()
    screen = MAP_SCREEN.read_text()
    assert "MAP_TILE_GLOBALSIGN_ROOT_R3" in ca
    assert "MAP_TILE_ISRG_ROOT_X1" in ca
    assert "GlobalSign Root CA - R3" in ca
    assert "ISRG Root X1" in ca
    assert ca.count("-----BEGIN CERTIFICATE-----") == 2
    assert "MAP_TILE_GLOBALSIGN_ROOT_R3" in screen
    assert "MAP_TILE_CA_BUNDLE" not in screen
    certificates = re.findall(
        r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", ca, re.S)
    fingerprints = []
    for certificate in certificates:
        parsed = subprocess.run(
            ["openssl", "x509", "-noout", "-fingerprint", "-sha256"],
            input=certificate, text=True, capture_output=True, check=True)
        fingerprints.append(parsed.stdout.strip())
    assert any("96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:" in value
               for value in fingerprints)
    assert any("CB:B5:22:D7:B7:F1:27:AD:6A:01:13:86:5B:DF:1C:D4:" in value
               for value in fingerprints)


def test_downloader_is_explicitly_opt_in_and_wired_only_for_visible_misses():
    screen = MAP_SCREEN.read_text()
    settings = SETTINGS.read_text()
    assert "downloadTile(request, transport_epoch)" in screen
    assert "downloader_.enqueue(request.key, request.frame_epoch)" in screen
    assert "presenter_.frameEpoch() != request.frame_epoch" in screen
    assert "download_failed_frame_epoch_ == request.frame_epoch" in screen
    worker = screen[screen.index("void MapScreen::workerLoop()"):
                    screen.index("Pyxis::MapTileLoadResult MapScreen::loadTile")]
    assert "frame_drained" not in worker
    assert "screen_visible_.load(std::memory_order_acquire)" in worker
    assert "transport_close_epoch_.load(std::memory_order_acquire)" in worker
    assert "requests_released_ && screen_visible" in worker
    take_request = worker[worker.index("presenter_.takeRequest") - 160:
                          worker.index("presenter_.takeRequest") + 80]
    assert "downloads_enabled" not in take_request
    assert "should_retain_download_transport" not in take_request
    assert "download_transport_.disconnectIdle()" in worker
    assert "screen_visible_.store(true, std::memory_order_release)" in screen
    assert "screen_visible_.exchange(false, std::memory_order_acq_rel)" in screen
    assert "transport_close_epoch_.fetch_add(1U, std::memory_order_acq_rel)" in screen
    download = screen[screen.index("Pyxis::MapTileLoadResult MapScreen::downloadTile"):
                      screen.index("Pyxis::MapTileLoadResult MapScreen::readTile")]
    assert "!screen_visible_.load(std::memory_order_acquire)" in download
    assert "transport_close_epoch_.load(std::memory_order_acquire) !=" in download
    assert 'KEY_MAP_DOWNLOAD = "map_dl"' in settings
    assert "prefs.getBool(KEY_MAP_DOWNLOAD, false)" in settings
    assert "Download map tiles:" in settings
    status = screen[screen.index("void MapScreen::setStatusFor"):
                    screen.index("bool MapScreen::applyOneCompletion")]
    assert '"Tile ready"' in status
    assert '"Offline"' not in status


def test_recent_decoded_tiles_use_a_fixed_psram_lru_before_sd_decode():
    screen = MAP_SCREEN.read_text()
    header = (ROOT / "lib/tdeck_ui/UI/LXMF/MapScreen.h").read_text()
    cache = (ROOT / "lib/tdeck_ui/UI/LXMF/DecodedTileCache.h").read_text()
    assert "CAPACITY = 12U" in cache
    assert "decoded_tile_cache_.get" in screen
    read_tile = screen[screen.index("Pyxis::MapTileLoadResult MapScreen::readTile("):
                       screen.index("Pyxis::MapTileLoadResult MapScreen::readCompressedTile(")]
    assert read_tile.index("decoded_tile_cache_.get") < read_tile.index("MapTileLookupPolicy::readLocal")
    assert "decoded_tile_cache_.put" in screen
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in screen
    assert "decoded_cache_pixels_[Pyxis::DecodedTileCache::CAPACITY]" in header
    assert "heap_caps_free(decoded_cache_pixels_[index])" in screen
    constructor = screen[screen.index("MapScreen::MapScreen"):
                         screen.index("MapScreen::~MapScreen")]
    staging = "compressed_staging_ = static_cast<std::uint8_t*>(heap_caps_malloc("
    cache_loop = "index < Pyxis::DecodedTileCache::CAPACITY; ++index)"
    assert constructor.index(staging) < constructor.index(cache_loop)
    assert "if (compressed_staging_)" in constructor
    start_worker = screen[screen.index("bool MapScreen::startWorker"):
                          screen.index("void MapScreen::stopWorker")]
    assert "heap_caps_malloc" not in start_worker
    assert "heap_caps_free(compressed_staging_)" not in start_worker


def test_settings_save_defers_persistence_and_application_outside_lvgl():
    settings = SETTINGS.read_text()
    capture = settings[settings.index("void SettingsScreen::save_settings()"):
                       settings.index("void SettingsScreen::service_pending_save()")]
    service = settings[settings.index("void SettingsScreen::service_pending_save()"):
                       settings.index("void SettingsScreen::update_ui_from_settings()")]
    assert "Preferences" not in capture
    assert "_save_callback" not in capture
    assert "Preferences prefs" in service
    assert "_save_callback(settings)" in service
    assert "SAVE_APPLY_RETRY" in SETTINGS_H.read_text()
    assert "applied ? SAVE_IDLE : SAVE_APPLY_RETRY" in service
    assert "millis() - _apply_retry_at_ms" in service
    assert "_apply_retry_at_ms = millis() + 1000U" in service

    main = MAIN.read_text()
    callback = main[main.index("settings->set_save_callback"):
                    main.index("// Apply initial brightness", main.index("settings->set_save_callback"))]
    assert "-> bool" in callback
    assert "return false" in callback
    assert "return true" in callback
    assert callback.count("RouterLock router_lock") == 1
    assert "if (!ble_mem)" in callback
    assert callback.index("app_settings = new_settings") > callback.index("Failed to start BLE interface")
    assert callback.count("return false") >= 5
    assert "if (!tcp_interface_impl->start())" in callback
    assert callback.index("app_settings.tcp_enabled = new_settings.tcp_enabled") < callback.index("Failed to start LoRa interface")
    assert callback.index("app_settings.lora_enabled = new_settings.lora_enabled") < callback.index("Failed to start AutoInterface")
    assert callback.index("app_settings.auto_enabled = new_settings.auto_enabled") < callback.index("Failed to start BLE interface")

    update = UI_MANAGER.read_text()
    body = update[update.index("void UIManager::update()"):
                  update.index("void UIManager::refresh_current_screen()")]
    assert body.index("service_pending_save()") < body.index("LVGL_LOCK")
