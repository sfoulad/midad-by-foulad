cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Pinned to the same version platformio.ini's [env:default]/[env:sticky]/[env:slim]
# lib_deps use (bblanchon/ArduinoJson @ 7.4.2) so host tests exercise identical
# trimming/serialization behavior to the firmware build.
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
set(ARDUINOJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ArduinoJson)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(arabic_shaper)
add_subdirectory(midad_app_settings_load_plan)
add_subdirectory(ota_rollback_recovery_plan)
add_subdirectory(ota_rollback_digest_format)
add_subdirectory(foulad_opds_hooks_pure)
add_subdirectory(opds_filename)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(http_verified_fetch)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(ble_device_info_payload)
add_subdirectory(x4pro_power_button_gesture)
add_subdirectory(line_height_math)
add_subdirectory(http_url_origin)
add_subdirectory(apps_grid_layout)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(css_section_cache)
add_subdirectory(html_to_plain_text)
add_subdirectory(keyboard_extension)
add_subdirectory(settings_extension)
add_subdirectory(settings_enum_labels)
add_subdirectory(firmware_update_policy)
add_subdirectory(reading_heatmap_layout)
add_subdirectory(stats_list_layout)
add_subdirectory(ota_ca_anchors)

add_subdirectory(settings_category_grid)
