"""Regression contracts for published-release-only, persistence-safe web flashing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FLASHER = ROOT / "docs/flasher/index.html"
WORKFLOW = ROOT / ".github/workflows/release-firmware.yml"
AUDIT = ROOT / "tools/audit_release_build.py"
VERSION_SCRIPT = ROOT / "version.py"


def test_flasher_starts_disabled_until_a_published_release_is_selected():
    source = FLASHER.read_text()

    assert '<option value="" disabled selected>Loading published releases...</option>' in source
    assert "flashBtn.disabled = true;" in source
    assert "let firmwarePathPrefix = null;" in source
    assert "firmwarePathPrefix = 'firmware/';" not in source
    assert '<option value="latest">' not in source


def test_flasher_filters_drafts_and_prereleases_and_selects_a_versioned_path():
    source = FLASHER.read_text()

    assert "if (release.draft || release.prerelease) continue;" in source
    assert "RELEASE_METADATA_ASSET = 'pyxis-release.json'" in source
    assert "if (!assetNames.includes(RELEASE_METADATA_ASSET)) continue;" in source
    assert "firmware/releases/${release.tag_name}/" in source
    assert "selectPublishedRelease" in source
    assert "flashBtn.disabled = false;" in source


def test_flasher_validates_release_metadata_and_downloaded_image_digests():
    source = FLASHER.read_text()

    assert "loadReleaseMetadata" in source
    assert "metadata.persistence_safe !== true" in source
    assert "metadata.version !== release.tag_name" in source
    assert "metadata.environment !== 'tdeck-release'" in source
    assert "crypto.subtle.digest('SHA-256'" in source
    assert "SHA-256 mismatch" in source
    assert "selectedReleaseMetadata" in source
    assert "metadata.images['bootloader.bin'].size > 0x8000" in source
    assert "metadata.images['partitions.bin'].size > 0x1000" in source


def test_custom_firmware_upload_is_explicit_validated_and_update_only():
    source = FLASHER.read_text()

    assert 'id="custom-firmware"' in source
    assert 'id="custom-firmware-status"' in source
    assert 'accept=".bin,application/octet-stream"' in source
    assert "validateCustomFirmware" in source
    assert "sha256Fallback" in source
    assert "crypto?.subtle" in source
    assert "Reading firmware.bin" in source
    assert "firmware.bin exceeds the app0 partition" in source
    assert "not an ESP32-S3 application image" in source
    assert "validateEspImageStructure" in source
    assert "invalid ESP image checksum" in source
    assert "invalid appended SHA-256" in source
    assert "customFirmwareBytes" in source
    assert "Custom firmware update only" in source
    assert "Flash Custom Firmware" in source
    assert "eraseCheckbox.disabled = true;" in source
    assert "const useFullInstall = customFirmwareBytes ? false : eraseCheckbox.checked;" in source


def test_custom_firmware_selection_invalidates_stale_async_results():
    source = FLASHER.read_text()
    handler = source[source.index("customFirmwareInput.addEventListener('change'"):source.index("function selectPublishedRelease")]

    assert "let customFirmwareSelectionToken = 0;" in source
    assert "const selectionToken = ++customFirmwareSelectionToken;" in handler
    assert handler.index("customFirmwareBytes = null;") < handler.index("await file.arrayBuffer()")
    assert handler.index("flashBtn.disabled = true;") < handler.index("await file.arrayBuffer()")
    assert handler.count("if (selectionToken !== customFirmwareSelectionToken) return;") >= 3
    assert "customFirmwareSelectionToken++;" in source[source.index("function selectPublishedRelease"):]
    assert "if (customFirmwareSelectionToken === 0) {" in source
    assert "versionSelect.options[0].textContent = 'Select a published release...'" in source


def test_rom_connection_timeout_explains_manual_boot_sequence_and_cleans_up():
    source = FLASHER.read_text()

    assert "CONNECT_TIMEOUT_MS" in source
    assert "const chip = await withTimeout(" in source
    assert "esploader.main()," in source
    assert "hold BOOT, tap RESET, release BOOT" in source
    assert "await transport.disconnect()" in source


def test_connected_rom_chip_is_verified_before_any_flash_write():
    source = FLASHER.read_text()

    assert "const EXPECTED_CHIP = 'ESP32-S3';" in source
    assert "const chipName = esploader.chip?.CHIP_NAME;" in source
    assert "if (chipName !== EXPECTED_CHIP)" in source
    assert "Wrong device: expected" in source
    assert source.index("if (chipName !== EXPECTED_CHIP)") < source.index("await esploader.writeFlash")


def test_flasher_fails_closed_when_published_releases_cannot_be_loaded():
    source = FLASHER.read_text()

    assert "No persistence-safe published firmware releases are available." in source
    assert "Unable to load published firmware releases." in source
    assert "versionSelect.disabled = true;" in source


def test_tag_builds_cannot_overwrite_pages_and_only_main_deploys():
    workflow = WORKFLOW.read_text()

    assert workflow.count("if: github.ref == 'refs/heads/main'") >= 4
    assert "select(.draft == false and .prerelease == false)" in workflow
    assert 'select(any(.assets[]; .name == "pyxis-release.json"))' in workflow
    assert 'validate_pyxis_web_release.py --directory "${dir}" --version "${tag}"' in workflow
    assert 'rm -rf "${dir}"' in workflow
    assert "fetch-depth: 0" in workflow


def test_valid_tag_deploys_only_its_versioned_assets_for_later_publication():
    workflow = WORKFLOW.read_text()

    assert "Deploy versioned web-flasher assets" in workflow
    assert "publish_dir: ./docs/flasher/firmware/releases/${{ steps.version.outputs.VERSION }}" in workflow
    assert "destination_dir: flasher/firmware/releases/${{ steps.version.outputs.VERSION }}" in workflow
    assert 'cp docs/flasher/firmware/pyxis-release.json "docs/flasher/firmware/releases/${VERSION}/"' in workflow


def test_tag_release_includes_audited_persistence_safety_metadata():
    workflow = WORKFLOW.read_text()

    assert "Generate audited release metadata" in workflow
    assert '"persistence_safe": true' in workflow
    assert '"source_commit": "${GITHUB_SHA}"' in workflow
    assert "pyxis-release.json" in workflow
    assert "sha256sum" in workflow
    for image in ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"):
        assert f'"{image}"' in workflow
    assert "python tools/validate_pyxis_web_release.py" in workflow


def test_release_tag_must_point_at_current_main_head():
    workflow = WORKFLOW.read_text()

    assert "Verify release tag points at current main" in workflow
    assert 'test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"' in workflow


def test_release_tag_values_are_validated_and_not_interpolated_into_shell_scripts():
    workflow = WORKFLOW.read_text()

    assert "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$" in workflow
    assert 'VERSION="${{ steps.version.outputs.VERSION }}"' not in workflow
    assert "VERSION: ${{ steps.version.outputs.VERSION }}" in workflow


def test_release_audit_rejects_stale_version_and_destructive_storage_firmware():
    audit = AUDIT.read_text()
    version_script = VERSION_SCRIPT.read_text()

    assert 'b"Firmware: v1.0.0"' in audit
    assert 'b"FileSystem mount failed; preserving persistent data"' in audit
    assert '"git", "describe", "--tags", "--always", "--dirty"' in audit
    assert 'os.environ.get("PYXIS_VERSION_OVERRIDE")' in audit
    assert 'f"Firmware: {expected_version}".encode()' in audit
    assert '"describe", "--tags", "--always", "--dirty"' in version_script
    assert 'os.environ.get("PYXIS_VERSION_OVERRIDE")' in version_script
