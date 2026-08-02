from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_tdeck_release_removes_diagnostic_flags():
    config = (ROOT / "platformio.ini").read_text()
    release = config.split("[env:tdeck-release]", 1)[1].split("[env:", 1)[0]

    assert "extends = env:tdeck" in release
    for flag in (
        "-DPYXIS_TEST_HOOKS",
        "-DPYXIS_TEST_TCP_HOST=*",
        "-DPYXIS_TEST_TCP_PORT=*",
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
