cmake_minimum_required(VERSION 3.15)
project(nomadnet_lvgl_acceptance LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT PYXIS_ROOT OR NOT LVGL_SOURCE)
  message(FATAL_ERROR "PYXIS_ROOT and LVGL_SOURCE are required")
endif()
set(LV_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE}" lvgl)
set(NN "${PYXIS_ROOT}/lib/tdeck_ui/UI/LXMF")
add_executable(nomadnet_lvgl_acceptance
  acceptance.cpp
  "${NN}/NomadNetDocument.cpp"
  "${NN}/NomadNetCompactPage.cpp"
  "${NN}/NomadNetForm.cpp"
  "${NN}/NomadNetGlyphs.cpp"
  "${NN}/NomadNetLibrary.cpp"
  "${NN}/NomadNetScreen.cpp"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_12.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_12_bold.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_12_italic.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_12_bold_italic.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_16.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_16_bold.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_16_italic.c"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts/nomadnet_font_16_bold_italic.c"
)
target_include_directories(nomadnet_lvgl_acceptance PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/stubs" "${NN}" "${PYXIS_ROOT}/lib/tdeck_ui/UI"
  "${PYXIS_ROOT}/lib/tdeck_ui/UI/Fonts"
  "${LVGL_SOURCE}" "${LVGL_SOURCE}/src")
target_compile_definitions(nomadnet_lvgl_acceptance PRIVATE ARDUINO=1)
target_compile_options(nomadnet_lvgl_acceptance PRIVATE -Wall -Wextra
  -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(nomadnet_lvgl_acceptance PRIVATE -fsanitize=address,undefined)
target_link_libraries(nomadnet_lvgl_acceptance PRIVATE lvgl)
