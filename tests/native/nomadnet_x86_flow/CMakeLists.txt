cmake_minimum_required(VERSION 3.15)
project(pyxis_nomadnet_x86_flow LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RNS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RNS_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)
set(RNS_BUILD_INTEROP OFF CACHE BOOL "" FORCE)
set(RNS_USE_PROVISIONING OFF CACHE BOOL "" FORCE)
# Native PosixFileSystem does not remap microReticulum's absolute
# /path_store prefix into a test directory. Keep route state in memory here;
# Pyxis Library persistence is still executed by the integration scenarios.
set(RNS_PERSIST_PATHS OFF CACHE BOOL "" FORCE)
if(NOT DEFINED RNS_SOURCE)
  message(FATAL_ERROR "RNS_SOURCE must name the pinned microReticulum checkout")
endif()
add_subdirectory("${RNS_SOURCE}" rns)

# These definitions must compile UDPInterface.cpp itself. The dependency's
# interop CMake applies them only to executables, leaving the library on 4242.
target_compile_definitions(udp_interface PRIVATE
  DEFAULT_UDP_LOCAL_HOST="127.0.0.1"
  DEFAULT_UDP_REMOTE_HOST="127.0.0.1"
  DEFAULT_UDP_LOCAL_PORT=14357
  DEFAULT_UDP_REMOTE_PORT=14356
)

set(PYXIS_NOMADNET_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/tdeck_ui/UI/LXMF")
set(PYXIS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
add_executable(pyxis_nomadnet_x86_flow
  client.cpp
  "${PYXIS_ROOT}/src/TCPClientInterface.cpp"
  "${PYXIS_NOMADNET_DIR}/NomadNetDocument.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetCompactPage.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetForm.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetGlyphs.cpp"
  "${PYXIS_NOMADNET_DIR}/NomadNetLibrary.cpp"
  "${PYXIS_NOMADNET_DIR}/NomadNetUrl.cpp"
)
target_include_directories(pyxis_nomadnet_x86_flow PRIVATE
  "${PYXIS_NOMADNET_DIR}"
  "${PYXIS_ROOT}/src"
)
target_link_libraries(pyxis_nomadnet_x86_flow PRIVATE microReticulum udp_interface)
target_compile_options(pyxis_nomadnet_x86_flow PRIVATE
  -Wall -Wextra -Werror
  -Wno-error=cpp
  -Wno-error=sign-compare
  -Wno-error=unused-parameter
  -Wno-error=missing-field-initializers
)
