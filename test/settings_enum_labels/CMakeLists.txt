add_executable(SettingsEnumLabelsTest
  SettingsEnumLabelsTest.cpp
)

# stubs/ comes first so its HalStorage.h / PersistableStore.h / I18n.h shadow
# the firmware ones; CrossPointSettings.h and SettingsEnumLabels.h themselves
# are the REAL sources, which is what makes the enum counts here authoritative.
target_include_directories(SettingsEnumLabelsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(SettingsEnumLabelsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(SettingsEnumLabelsTest)
