cmake_minimum_required(VERSION 3.14)
project(PyxisMicroLXMFTelemetryFieldTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED MICROLXMF_BRIDGE_DIR)
    message(FATAL_ERROR "MICROLXMF_BRIDGE_DIR is required")
endif()
if(NOT DEFINED MICROLXMF_SRC)
    message(FATAL_ERROR "MICROLXMF_SRC is required")
endif()
if(NOT DEFINED PYXIS_ROOT)
    message(FATAL_ERROR "PYXIS_ROOT is required")
endif()

add_subdirectory(${MICROLXMF_BRIDGE_DIR} microlxmf-bridge)

add_executable(test_pyxis_telemetry_field
    test_telemetry_field_roundtrip.cpp
    ${PYXIS_ROOT}/lib/tdeck_ui/Telemetry/LocationTelemetryCodec.cpp
)
target_link_libraries(test_pyxis_telemetry_field PRIVATE MicroLXMFLib)
target_include_directories(test_pyxis_telemetry_field PRIVATE
    ${PYXIS_ROOT}/lib/tdeck_ui
)
target_compile_definitions(test_pyxis_telemetry_field PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)
target_compile_options(test_pyxis_telemetry_field PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-cpp
    -Wno-missing-field-initializers
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
)
target_link_options(test_pyxis_telemetry_field PRIVATE
    -fsanitize=address,undefined
)

# Host-side benchmark of the conversation-list store load path (the per-
# conversation work ConversationListScreen::refresh() performs). Builds
# against the exact pinned microLXMF source, so before/after numbers are
# comparable across pin bumps. BENCH_INDEX_ACCESSORS is added by the
# harness only when the pinned store supports in-memory preview accessors.
add_executable(bench_conversation_list_load
    bench_conversation_list_load.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(bench_conversation_list_load PRIVATE MicroLXMFLib Threads::Threads)
target_compile_options(bench_conversation_list_load PRIVATE
    -fexceptions
    -Wall -Wno-unused-parameter -Wno-missing-field-initializers
)
target_compile_definitions(bench_conversation_list_load PRIVATE
    MICROLXMF_NATIVE=1
    NATIVE=1
)
# BENCH_INDEX_ACCESSORS=1 only when the pinned store exposes the in-memory
# preview accessors (get_last_message_preview); otherwise the binary reports
# that it is baseline-only.
option(BENCH_INDEX_ACCESSORS "pin supports in-memory preview accessors" OFF)
if(BENCH_INDEX_ACCESSORS)
    target_compile_definitions(bench_conversation_list_load PRIVATE BENCH_INDEX_ACCESSORS=1)
endif()
