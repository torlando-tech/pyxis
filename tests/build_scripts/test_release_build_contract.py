from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MICROSTORE_PIN = "https://github.com/attermann/microStore.git#c5fb69d68229e684c7fbd17692a67ae8193b84e2"
MICRORETICULUM_PIN = "https://github.com/torlando-tech/microReticulum.git#1bbd422a3b25b4a710642fc5cd01039e5774a4a7"


def test_microstore_pin_resolves_before_transitive_registry_requirement():
    config = (ROOT / "platformio.ini").read_text()
    dependencies = config.split("lib_deps =", 1)[1].split("; Build configuration", 1)[0]

    assert dependencies.count(MICROSTORE_PIN) == 1
    assert dependencies.index(MICROSTORE_PIN) < dependencies.index(MICRORETICULUM_PIN)


def test_release_auditor_expects_the_configured_microreticulum_pin():
    audit = (ROOT / "tools/audit_release_build.py").read_text()
    expected_revision = MICRORETICULUM_PIN.rsplit("#", 1)[1]

    assert f'"microReticulum": "{expected_revision}"' in audit


def test_tdeck_release_removes_diagnostic_flags():
    config = (ROOT / "platformio.ini").read_text()
    release = config.split("[env:tdeck-release]", 1)[1].split("[env:", 1)[0]

    assert "extends = env:tdeck" in release
    for flag in (
        "-DPYXIS_TEST_HOOKS",
        "-DPYXIS_TEST_TCP_HOST",
        "-DPYXIS_TEST_TCP_PORT",
        "-DMEMORY_INSTRUMENTATION_ENABLED",
        "-DBOOT_PROFILING_ENABLED",
    ):
        assert flag in release


def test_disabled_instrumentation_macros_remain_defined():
    main = (ROOT / "src/main.cpp").read_text()
    memory = (ROOT / "lib/microreticulum-shim/Instrumentation/MemoryMonitor.h").read_text()
    boot = (ROOT / "lib/microreticulum-shim/Instrumentation/BootProfiler.h").read_text()

    assert "#include <Instrumentation/MemoryMonitor.h>" in main
    assert "#include <Instrumentation/BootProfiler.h>" in main
    assert "#define MEMORY_MONITOR_POLL() ((void)0)" in memory
    assert "#define BOOT_PROFILE_COMPLETE() ((void)0)" in boot


def test_ci_and_deployment_build_the_release_environment():
    build_check = (ROOT / ".github/workflows/build-check.yml").read_text()
    release = (ROOT / ".github/workflows/release-firmware.yml").read_text()

    assert "environment: [tdeck, tdeck-release]" in build_check
    assert "matrix.environment == 'tdeck-release'" in build_check
    assert ".pio/build/tdeck-release/firmware.bin" in build_check
    assert "python tools/audit_release_build.py" in build_check

    assert "pio run -e tdeck-release" in release
    assert "python tools/audit_release_build.py" in release
    assert ".pio/build/tdeck-release/bootloader.bin" in release
    assert ".pio/build/tdeck-release/partitions.bin" in release
    assert ".pio/build/tdeck-release/firmware.bin" in release
    assert ".pio/build/tdeck/firmware.bin" not in release
