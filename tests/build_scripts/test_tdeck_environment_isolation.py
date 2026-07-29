from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "platformio.ini"


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
    assert "PYXIS_TEST_TCP_HOST" in instrumented
    assert "PYXIS_TEST_TCP_PORT" in instrumented
