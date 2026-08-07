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
