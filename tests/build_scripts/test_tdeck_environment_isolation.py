from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "platformio.ini"
MAIN = ROOT / "src/main.cpp"
HARDWARE_RUNNER = ROOT / "tests/hardware/run_e2e.sh"
VOICE_README = ROOT / "tools/voice_test/README.md"


def section(text: str, name: str) -> str:
    start = text.index(f"[env:{name}]")
    end = text.find("\n[env:", start + 1)
    return text[start:] if end < 0 else text[start:end]


def test_test_hooks_are_isolated_from_production_tdeck():
    config = CONFIG.read_text()
    production = section(config, "tdeck")
    instrumented = section(config, "tdeck-test")
    assert "PYXIS_TEST_HOOKS" not in production
    assert "PYXIS_TEST_TCP_HOST" not in production
    assert "PYXIS_TEST_TCP_PORT" not in production
    assert "extends = env:tdeck" in instrumented
    assert "-DPYXIS_TEST_HOOKS" in instrumented
    assert '\'-DPYXIS_TEST_TCP_HOST="${sysenv.PYXIS_TEST_TCP_HOST}"\'' in instrumented
    assert '\'-DPYXIS_TEST_TCP_PORT="${sysenv.PYXIS_TEST_TCP_PORT}"\'' in instrumented


def test_audio_diagnostic_state_and_recorder_initialization_are_test_only():
    main = MAIN.read_text()
    assert "#ifdef PYXIS_TEST_HOOKS\n// --- Audio loopback PCM dump" in main
    mutex = main.index("g_rec_mutex = xSemaphoreCreateMutex()")
    assert main.rfind("#ifdef PYXIS_TEST_HOOKS", 0, mutex) > main.rfind("#endif", 0, mutex)


def test_test_hook_defaults_and_harness_target_are_safe():
    main = MAIN.read_text()
    hook_defaults = main[main.index("#ifdef PYXIS_TEST_HOOKS") : main.index("#include <Wire.h>")]
    assert '#define PYXIS_TEST_TCP_HOST ""' in hook_defaults
    assert '#define PYXIS_TEST_TCP_PORT ""' in hook_defaults

    runner = HARDWARE_RUNNER.read_text()
    assert 'PYXIS_ENV="${PYXIS_ENV:-tdeck-test}"' in runner

    voice_readme = VOICE_README.read_text()
    assert "pio run -e tdeck-test" in voice_readme
