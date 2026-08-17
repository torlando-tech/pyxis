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
set(PYXIS_MANIFEST_BASE "51e4b586c4c3867ae399f573557edda6f3b48a44")
set(PYXIS_MANIFEST_BRANCH "feat/nomadnet-partials-core")
set(PYXIS_MANIFEST_MICRORETICULUM "cd0338e7fc07d3a7785a450656ba766491cbf6e8")
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${RNS_SOURCE}"
  OUTPUT_VARIABLE _rns_commit OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _rns_git_result)
if(NOT _rns_git_result EQUAL 0 OR NOT _rns_commit STREQUAL PYXIS_MANIFEST_MICRORETICULUM)
  message(FATAL_ERROR "RNS_SOURCE is not the attested microReticulum commit")
endif()
set(PYXIS_MANIFEST_FILES
  "tests/native/nomadnet_x86_flow/CMakeLists.txt"
  "tests/native/nomadnet_x86_flow/BuildManifest.h.in"
  "tests/native/nomadnet_x86_flow/client.cpp"
  "src/TCPClientInterface.cpp"
  "src/TCPClientInterface.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetOwner.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetOwner.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetActionMailbox.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetHistory.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetDocument.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetDocument.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetCompactPage.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetCompactPage.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetPartialScheduler.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetPartialScheduler.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetPartialHash.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetGlyphs.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetGlyphs.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetForm.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetForm.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetLibrary.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetLibrary.h"
  "lib/tdeck_ui/UI/LXMF/NomadNetUrl.cpp"
  "lib/tdeck_ui/UI/LXMF/NomadNetUrl.h")
set(PYXIS_MANIFEST_SOURCES_JSON "{")
set(_manifest_separator "")
foreach(_relative IN LISTS PYXIS_MANIFEST_FILES)
  file(SHA256 "${PYXIS_ROOT}/${_relative}" _digest)
  string(APPEND PYXIS_MANIFEST_SOURCES_JSON "${_manifest_separator}\"${_relative}\":\"${_digest}\"")
  set(_manifest_separator ",")
endforeach()
string(APPEND PYXIS_MANIFEST_SOURCES_JSON "}")
configure_file(BuildManifest.h.in "${CMAKE_CURRENT_BINARY_DIR}/BuildManifest.h" @ONLY)
add_executable(pyxis_nomadnet_x86_flow
  client.cpp
  "${PYXIS_NOMADNET_DIR}/NomadNetOwner.cpp"
  "${PYXIS_ROOT}/src/TCPClientInterface.cpp"
  "${PYXIS_NOMADNET_DIR}/NomadNetDocument.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetCompactPage.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetPartialScheduler.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetForm.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF/NomadNetGlyphs.cpp"
  "${PYXIS_NOMADNET_DIR}/NomadNetLibrary.cpp"
  "${PYXIS_NOMADNET_DIR}/NomadNetUrl.cpp"
)
target_include_directories(pyxis_nomadnet_x86_flow PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
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
