"""Source-level regression checks for fail-closed LXMF persistence UI flow."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
UI_MANAGER = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
CHAT_SCREEN = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.cpp"
COMPOSE_SCREEN = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/ComposeScreen.cpp"
MAIN = REPO_ROOT / "src/main.cpp"


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_outgoing_send_is_published_off_lock_and_persisted_on_main_loop():
    # Regression: the LVGL send callback used to run identity recall,
    # RouterLock admission, and the LittleFS persistence under the LVGL lock.
    # A multi-second save then tripped the 5s deadlock guard (LVGLLock.h:45)
    # and rebooted the device. send_message must now stay allocation-light
    # and lock-free; the durable work belongs in service_pending_sends,
    # which update() services before LVGL_LOCK.
    source = UI_MANAGER.read_text()
    send_body = function_body(
        source,
        "bool UIManager::send_message(",
        "void UIManager::service_pending_sends(",
    )
    service_body = function_body(
        source,
        "void UIManager::service_pending_sends(",
        "void UIManager::apply_outbound_result(",
    )

    # LVGL-task side: publish only. No router lock, no persistence, no
    # router admission on this path.
    assert "_outgoing_sends.request(" in send_body
    assert "RouterLock" not in send_body
    assert "_router.try_handle_outbound(" not in send_body
    assert "_store.save_message(" not in send_body

    # Main-loop side: admission guard commits the final packed/stamped
    # message immediately before queue ownership transfer.
    lock = service_body.index("RouterLock router_lock(0)")
    admission = service_body.index("_router.try_handle_outbound(")
    assert lock < admission
    assert "persistOutgoingMessage" in service_body
    assert "Outgoing message persistence failed; message not queued" in service_body
    assert "The message was not sent" in source

    # update() services the mailbox before acquiring LVGL_LOCK.
    update_body = function_body(source, "void UIManager::update()", "bool UIManager::send_message(")
    assert update_body.index("service_pending_sends();") < update_body.index("LVGL_LOCK();")


def test_ui_messages_prefer_lora_safe_opportunistic_delivery():
    source = UI_MANAGER.read_text()
    body = function_body(
        source,
        "bool UIManager::send_message(",
        "void UIManager::on_message_received(",
    )
    assert "::LXMF::Type::Message::OPPORTUNISTIC" in body
    admission = (
        "_router.try_handle_outbound("
        if "_router.try_handle_outbound(" in body
        else "_router.handle_outbound(message)"
    )
    assert body.index("::LXMF::Type::Message::OPPORTUNISTIC") < body.index(admission)


def test_incoming_message_is_committed_before_display():
    source = UI_MANAGER.read_text()
    body = function_body(
        source,
        "void UIManager::on_message_received(",
        "void UIManager::on_message_delivered(",
    )

    save = body.index("if (!_store.save_message(message))")
    display = body.index("_chat_screen->add_message(message, false)")

    assert save < display
    assert "Incoming message persistence failed; message not added to history" in body
    assert "An incoming message could not be saved" in body


def test_no_message_save_result_is_silently_ignored():
    source = UI_MANAGER.read_text()
    assert "\n    _store.save_message(message);" not in source


def test_delivery_state_is_committed_before_ui_update():
    source = UI_MANAGER.read_text()
    main = MAIN.read_text()
    delivered = function_body(
        source,
        "void UIManager::on_message_delivered(",
        "void UIManager::on_message_failed(",
    )
    failed = function_body(
        source,
        "void UIManager::on_message_failed(",
        "void UIManager::refresh_current_screen(",
    )
    callback = function_body(
        main,
        "router->register_delivered_callback(",
        "// Boot profiling complete",
    )
    assert "if (!message_store->update_message_state(" in callback
    assert callback.index("message_store->update_message_state(") < callback.index(
        "ui_manager->on_message_delivered(full_msg)"
    )
    assert "_store.update_message_state(" not in delivered
    assert "_chat_screen->update_message_status(message.hash(), true)" in delivered
    assert failed.index("_store.update_message_state(") < failed.index(
        "_chat_screen->update_message_status(message.hash(), false)"
    )


def test_rejected_outgoing_message_keeps_retryable_input():
    chat = CHAT_SCREEN.read_text()
    compose = COMPOSE_SCREEN.read_text()
    ui = UI_MANAGER.read_text()
    # Both send callbacks publish through the main-loop handoff; the LVGL
    # handler itself no longer clears the input.
    assert "screen->_send_message_callback(message)" in chat
    assert "lv_textarea_set_text(screen->_text_area" not in chat.split(
        "void ChatScreen::on_send_clicked("
    )[1].split("}")[0]
    assert "if (screen->_send_callback && screen->_send_callback(dest_hash, message))" in compose
    # The composer is cleared only after persistence + admission succeed,
    # from the main-loop commit.
    apply_body = ui[ui.index("void UIManager::apply_outbound_result("):]
    assert "_chat_screen->clear_composer();" in apply_body


def test_storage_error_dialogs_are_coalesced():
    source = UI_MANAGER.read_text()
    assert "if (storage_error_dialog) return;" in source
    assert "storage_error_dialog = nullptr;" in source


def test_successful_boot_cancels_ota_rollback_after_subsystems_initialize():
    source = MAIN.read_text()
    confirm = function_body(
        source,
        "void confirm_running_firmware()",
        "void setup_lvgl_and_ui()",
    )
    assert "ESP_OTA_IMG_PENDING_VERIFY" in confirm
    assert "esp_ota_mark_app_valid_cancel_rollback()" in confirm
    setup = function_body(source, "void setup()", "void loop()")
    assert setup.index("setup_lxmf();") < setup.index("setup_ui_manager();")
    assert setup.index("setup_ui_manager();") < setup.index("LVGLInit::start_task")
    assert setup.index("LVGLInit::start_task") < setup.index("confirm_running_firmware();")


def test_system_info_reports_littlefs_not_unmounted_spiffs():
    # The live system readouts (firmware build, storage, RAM) moved from the
    # Settings screen to the Status screen; the contract follows them there.
    source = (REPO_ROOT / "lib/tdeck_ui/UI/LXMF/StatusScreen.cpp").read_text()
    assert '"Firmware: " FIRMWARE_VERSION' in source
    assert "LittleFS.totalBytes()" in source
    assert "LittleFS.usedBytes()" in source
    assert "SPIFFS.totalBytes()" not in source


def test_failed_littlefs_mount_enters_stable_recovery_mode_before_reticulum():
    source = MAIN.read_text()
    setup = function_body(source, "void setup()", "void loop()")
    loop = source[source.index("void loop()"):]

    assert "Storage::mount_or_initialize_erased_littlefs(" in source
    assert "[]() { return fs.init(false); }" in source
    assert """littlefs_partition = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA,
                ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                LITTLEFS_PARTITION_LABEL);""" in source
    assert 'static constexpr const char* LITTLEFS_PARTITION_LABEL = "spiffs";' in source
    assert 'true, "/littlefs", 10, LITTLEFS_PARTITION_LABEL' in source
    assert "Persistent storage unavailable" in source
    assert "USB serial recovery remains available." in source
    assert setup.index("if (!persistent_storage_ready)") < setup.index("setup_reticulum();")
    assert "enter_storage_recovery_mode();" in setup
    recovery_branch = setup[setup.index("if (!persistent_storage_ready)"):setup.index("return;", setup.index("if (!persistent_storage_ready)"))]
    assert recovery_branch.index("configure_loop_watchdog();") < recovery_branch.index("enter_storage_recovery_mode();")
    assert "if (storage_recovery_mode)" in loop
    assert loop.index("if (storage_recovery_mode)") < loop.index("reticulum->loop();")
