"""Source-level regression checks for production watchdog liveness."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MAIN = REPO_ROOT / "src/main.cpp"
AUTO_INTERFACE = REPO_ROOT / "lib/auto_interface/AutoInterface.cpp"
TCP_INTERFACE = REPO_ROOT / "src/TCPClientInterface.cpp"
LVGL_INIT = REPO_ROOT / "lib/tdeck_ui/UI/LVGL/LVGLInit.cpp"
BLE_INTERFACE = REPO_ROOT / "lib/ble_interface/BLEInterface.cpp"
CAPTURE = REPO_ROOT / "lib/lxst_audio/i2s_capture.cpp"
PLAYBACK = REPO_ROOT / "lib/lxst_audio/i2s_playback.cpp"


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_production_watchdog_panics_and_loop_task_is_subscribed():
    source = MAIN.read_text()
    setup = function_body(source, "void setup()", "// Serial command buffer")
    configure = function_body(
        source,
        "static void configure_loop_watchdog()",
        "void setup_reticulum()",
    )

    assert "ESP_ERROR_CHECK(esp_task_wdt_init(60, true));" in setup
    assert "esp_task_wdt_init(60, false)" not in setup
    assert setup.index("ESP_ERROR_CHECK(esp_task_wdt_init(60, true));") < setup.index(
        "setup_hardware();"
    )
    assert "esp_task_wdt_status(nullptr)" in configure
    assert "ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));" in configure
    assert "panic enabled" in configure


def test_main_and_lvgl_tasks_feed_and_yield():
    main = MAIN.read_text()
    loop = main[main.index("void loop()") :]
    lvgl = LVGL_INIT.read_text()
    lvgl_task = function_body(lvgl, "void LVGLInit::lvgl_task(", "bool LVGLInit::start_task(")

    assert loop.index("esp_task_wdt_reset();") < loop.index("ArduinoOTA.handle();")
    assert "RNS::Utilities::OS::set_loop_callback([]() { esp_task_wdt_reset(); });" in main
    assert "persistence_elapsed_ms > 1000" in loop
    assert "Reticulum persistence stalled loopTask for %lu ms (TWDT limit is 60000 ms)" in loop
    assert "delay(5);" in loop
    assert "esp_task_wdt_add(nullptr);" in lvgl_task
    assert "esp_task_wdt_reset();" in lvgl_task
    assert "vTaskDelay(pdMS_TO_TICKS(5));" in lvgl_task


def test_auto_interface_socket_drains_are_bounded():
    source = AUTO_INTERFACE.read_text()
    assert "static constexpr size_t SOCKET_RX_BUDGET = 16;" in source

    for signature, next_signature in (
        ("void AutoInterface::process_discovery()", "void AutoInterface::process_data()"),
        ("void AutoInterface::process_data()", "bool AutoInterface::setup_unicast_discovery_socket()"),
        (
            "void AutoInterface::process_unicast_discovery()",
            "void AutoInterface::reverse_announce(",
        ),
    ):
        body = function_body(source, signature, next_signature)
        assert "packet_count < SOCKET_RX_BUDGET" in body
        assert "while (true)" not in body


def test_tcp_socket_and_frame_processing_are_bounded():
    source = TCP_INTERFACE.read_text()
    loop_start = source.index("/*virtual*/ void TCPClientInterface::loop()")
    loop_end = source.index("\n#endif", loop_start) + len("\n#endif")
    arduino_loop = source[loop_start:loop_end]
    frames = function_body(
        source,
        "void TCPClientInterface::extract_and_process_frames()",
        "/*virtual*/ bool TCPClientInterface::send_outgoing(",
    )

    assert "static constexpr size_t MAX_TCP_BYTES_PER_LOOP = 4096;" in source
    assert "static constexpr size_t MAX_TCP_FRAMES_PER_LOOP = 32;" in source
    assert "static constexpr size_t MAX_TCP_FRAME_BUFFER = 16384;" in source
    assert "extract_and_process_frames();" in arduino_loop
    assert "_frame_buffer.size() >= MAX_TCP_FRAME_BUFFER" in arduino_loop
    assert "std::min(MAX_TCP_BYTES_PER_LOOP, room)" in arduino_loop
    assert "bytes_read < read_budget" in arduino_loop
    assert arduino_loop.index("extract_and_process_frames();") < arduino_loop.index(
        "_client.available()"
    )
    assert "frame_budget < MAX_TCP_FRAMES_PER_LOOP" in frames
    assert "while (true)" not in frames


def test_worker_tasks_are_watched_or_yield_with_bounded_timeouts():
    ble = BLE_INTERFACE.read_text()
    capture = CAPTURE.read_text()
    playback = PLAYBACK.read_text()
    tcp = TCP_INTERFACE.read_text()

    ble_task = function_body(ble, "void BLEInterface::ble_task(", "bool BLEInterface::start_task(")
    tcp_task = function_body(tcp, "void TCPClientInterface::task_loop()", "#endif")

    assert "ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));" in ble_task
    assert ble_task.count("esp_task_wdt_reset();") >= 2
    assert "feed_ble_watchdog_if_owner(_task_handle);" in ble
    assert "vTaskDelay(pdMS_TO_TICKS(10));" in ble_task
    assert "vTaskDelay(pdMS_TO_TICKS(100));" in tcp_task
    assert "i2s_read(" in capture and "pdMS_TO_TICKS(100)" in capture
    assert "i2s_write(" in playback and "pdMS_TO_TICKS(100)" in playback
    assert "vTaskDelay(pdMS_TO_TICKS(5));" in playback


def test_tcp_connect_worker_does_not_silently_starve_below_heap_threshold():
    source = TCP_INTERFACE.read_text()
    tcp_task = function_body(source, "void TCPClientInterface::task_loop()", "#endif")

    # The complete T-Deck UI can have a largest internal allocation below 20 KiB
    # while remaining healthy. A fixed heap gate here permanently suppresses
    # every initial/reconnect attempt, leaving persisted routes pointed at an
    # offline interface. Safety comes from the task-side bounded connect timeout
    # and retry cadence, not from silently skipping connect().
    assert "ESP.getMaxAllocHeap()" not in tcp_task
    assert "_conn_state.store(CONNECTING)" in tcp_task
    assert "if (connect())" in tcp_task
    assert "RECONNECT_WAIT_MS" in tcp_task
    assert "vTaskDelay(pdMS_TO_TICKS(100));" in tcp_task


def test_tcp_connect_worker_restart_rearms_join_completion_latch():
    source = TCP_INTERFACE.read_text()
    start = function_body(
        source,
        "/*virtual*/ bool TCPClientInterface::start()",
        "bool TCPClientInterface::connect()",
    )
    stop = function_body(
        source,
        "/*virtual*/ void TCPClientInterface::stop()",
        "/*virtual*/ void TCPClientInterface::loop()",
    )

    # Runtime settings reuse the interface with stop() -> start() -> stop().
    # Every new worker must re-arm the completion latch before publication, or
    # the second stop can mistake the prior worker's completion for the current
    # worker and disconnect/free its socket and owner without joining it.
    assert "_task_done = false;" in start
    assert start.index("_task_done = false;") < start.index("_task_running = true;")
    assert start.index("_task_done = false;") < start.index("xTaskCreatePinnedToCore(")
    assert "while (!_task_done" in stop


def test_tcp_reconnect_reports_typed_stages_and_bounds_wifi_reassociation():
    source = TCP_INTERFACE.read_text()
    header = (REPO_ROOT / "src" / "TCPClientInterface.h").read_text()
    main = MAIN.read_text()
    connect = function_body(
        source,
        "bool TCPClientInterface::connect()",
        "bool TCPClientInterface::configure_socket()",
    )
    worker = function_body(source, "void TCPClientInterface::task_loop()", "#endif")

    for stage in (
        "DNS", "SOCKET", "CONNECT", "SELECT_TIMEOUT", "SELECT_ERROR",
        "SO_ERROR_READ", "SOCKET_ERROR", "SOCKET_OPTIONS",
    ):
        assert f'TcpConnectStage::{stage}' in connect
    assert "WiFi.hostByName" in connect
    dns_failure = connect[connect.index("if (!WiFi.hostByName"):
                          connect.index("int sockfd = socket", connect.index("if (!WiFi.hostByName"))]
    assert "record_connect_failure(TcpConnectStage::DNS, 0, false);" in dns_failure
    assert "socket(AF_INET, SOCK_STREAM, 0)" in connect
    assert "::connect(" in connect
    assert "select(" in connect
    assert "getsockopt(" in connect
    assert "_client = WiFiClient(sockfd);" in connect
    assert '"T:TCP_FAILURE stage=%s' in source

    assert "set_operation_active_callback" in header
    assert "LOCAL_FAILURES_BEFORE_REASSOCIATE = 3" in header
    assert "REASSOCIATE_COOLDOWN_MS = 120000" in header
    assert "REASSOCIATE_MIN_INTERNAL_FREE = 24 * 1024" in header
    assert "REASSOCIATE_MIN_LARGEST_BLOCK = 12 * 1024" in header
    assert "maybe_reassociate_wifi();" in worker
    assert "WiFi.reconnect()" in source
    assert "_operation_active && _operation_active()" in source
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in source
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)" in source
    assert "void create_tcp_interface()" in main
    assert main.count('new TCPClientInterface("tcp0")') == 1
    assert "set_operation_active_callback([]" in main
    assert "ui_manager && ui_manager->nomad_operation_active()" in main
    assert main.count("create_tcp_interface();") >= 3


def test_registered_interfaces_have_one_polling_owner():
    main = MAIN.read_text()
    loop = main[main.index("void loop()") :]
    ble = BLE_INTERFACE.read_text()
    ble_loop = function_body(
        ble,
        "void BLEInterface::loop()",
        "//=============================================================================\n// Data Transfer",
    )

    # Reticulum polls registered TCP/LoRa interfaces. The main loop must not
    # invoke them a second time after reticulum->loop().
    assert "reticulum->loop();" in loop
    assert "tcp_interface->loop();" not in loop
    assert "lora_interface->loop();" not in loop
    assert "ble_interface->loop();" not in loop

    # Once started, only the dedicated BLE worker may enter blocking GATT work;
    # Reticulum's loopTask-side interface poll must return immediately.
    assert "xTaskGetCurrentTaskHandle() != _task_handle" in ble_loop
    assert main.count("ble_interface_impl->start_task(1, 0)") >= 2
    assert "feed_ble_watchdog_if_owner(const void* owner)" in ble
    assert "return;" in ble_loop
