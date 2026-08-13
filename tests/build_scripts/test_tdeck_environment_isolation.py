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


def test_nomad_link_diagnostic_is_minimal_and_separate_from_broad_hooks():
    config = CONFIG.read_text()
    production = section(config, "tdeck")
    diagnostic = section(config, "tdeck-nomad-link-diagnostic")

    assert "extends = env:tdeck" in diagnostic
    assert "-DPYXIS_NOMAD_LINK_DIAGNOSTIC" in diagnostic
    assert "-DPYXIS_TEST_HOOKS" not in diagnostic
    assert "PYXIS_TEST_TCP_HOST" not in diagnostic
    assert "PYXIS_TEST_TCP_PORT" not in diagnostic
    assert "PYXIS_NOMAD_LINK_DIAGNOSTIC" not in production


def test_nomad_link_diagnostic_uses_fixed_buffer_status_output():
    main = MAIN.read_text()
    manager = (ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp").read_text()
    header = (ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.h").read_text()

    assert "#ifdef PYXIS_NOMAD_LINK_DIAGNOSTIC" in main
    assert "T:NOMAD_STATUS" in main
    assert "void test_nomad_status() const;" in header
    status_start = manager.index("void UIManager::test_nomad_status() const")
    status = manager[status_start : status_start + 2200]
    assert "Serial.printf(" in status
    assert "std::string(" not in status
    assert "std::to_string(" not in status


def test_nomad_link_crypto_vector_uses_pinned_wrappers_without_secret_output():
    config = CONFIG.read_text()
    main = MAIN.read_text()
    diagnostic = section(config, "tdeck-nomad-link-diagnostic")
    production = section(config, "tdeck")

    assert "-DPYXIS_NOMAD_LINK_DIAGNOSTIC" in diagnostic
    assert "PYXIS_NOMAD_LINK_DIAGNOSTIC" not in production
    assert 'line == "T:CRYPTO_VECTOR"' in main
    assert "run_nomad_crypto_vector()" in main
    assert "X25519PrivateKey::from_private_bytes" in main
    assert "Ed25519PrivateKey::from_private_bytes" in main
    assert "RNS::Cryptography::hkdf" in main
    assert 'Serial.printf("T:CRYPTO_VECTOR %s xpub=%d xdh=%d edpub=%d edsig=%d hkdf=%d' in main

    vector_start = main.index("static void run_nomad_crypto_vector()")
    vector_end = main.index("static void handle_nomad_link_diagnostic_command", vector_start)
    vector_body = main[vector_start:vector_end]
    assert ".toHex()" not in vector_body
    assert "private=" not in vector_body
    assert "shared=" not in vector_body


def test_nomad_link_wire_capture_is_fixed_buffer_and_arduino_diagnostic_only():
    source = (ROOT / "src/TCPClientInterface.cpp").read_text()
    assert "#if defined(ARDUINO) && defined(PYXIS_NOMAD_LINK_DIAGNOSTIC)" in source
    assert 'nomad_trace_packet("TX", data' in source
    assert 'nomad_trace_packet("RX", unescaped' in source
    assert 'Serial.printf("T:WIRE %s kind=%s raw=%u framed=%u io=%u hex="' in source
    assert "std::string hex" not in source[source.index("static void nomad_trace_packet") : source.index("TCPClientInterface::TCPClientInterface")]
    assert "InterfaceImpl::handle_incoming(unescaped);" in source


def test_nomad_harness_extracts_machine_response_from_interleaved_serial_line():
    harness = (ROOT / "tests/hardware/nomadnet_tdeck_harness.py").read_text()

    assert "for prefix in prefixes:" in harness
    assert "marker = line.find(prefix)" in harness
    assert "return line[marker:], observed" in harness


def test_audio_diagnostic_state_and_recorder_initialization_are_test_only():
    main = MAIN.read_text()
    assert "#ifdef PYXIS_TEST_HOOKS\n// --- Audio loopback PCM dump" in main
    mutex = main.index("g_rec_mutex = xSemaphoreCreateMutex()")
    assert main.rfind("#ifdef PYXIS_TEST_HOOKS", 0, mutex) > main.rfind("#endif", 0, mutex)


def test_udp_log_callback_does_not_reenter_wifi_status_lock():
    main = MAIN.read_text()
    start = main.index("static void udp_send(")
    end = main.index("extern \"C\" void pyxis_log", start)
    body = main[start:end]
    assert "WiFi.status()" not in body
    assert "O_NONBLOCK" in main[main.index("static void udp_log_init()") : start]
    assert "WIFI_STAGE:" not in main
    udp_setup = main.index("udp_log_init();", main.index("void on_wifi_connected()"))
    assert main.rfind("#ifdef PYXIS_TEST_HOOKS", 0, udp_setup) > main.rfind("#endif", 0, udp_setup)
    heap_diag = main.index("static uint32_t last_heap_check")
    assert main.rfind("#ifdef PYXIS_TEST_HOOKS", 0, heap_diag) > main.rfind("#endif", 0, heap_diag)
    setup_start = main.index("void setup_wifi()")
    setup_wifi = main[setup_start : main.index("// One-shot post-WiFi-connect", setup_start)]
    assert "on_wifi_connected();" not in setup_wifi
    assert "post-connect services deferred" in setup_wifi


def test_test_hook_defaults_and_harness_target_are_safe():
    main = MAIN.read_text()
    hook_defaults = main[main.index("#ifdef PYXIS_TEST_HOOKS") : main.index("#include <Wire.h>")]
    assert '#define PYXIS_TEST_TCP_HOST ""' in hook_defaults
    assert '#define PYXIS_TEST_TCP_PORT ""' in hook_defaults

    runner = HARDWARE_RUNNER.read_text()
    assert 'PYXIS_ENV="${PYXIS_ENV:-tdeck-test}"' in runner

    voice_readme = VOICE_README.read_text()
    assert "pio run -e tdeck-test" in voice_readme
