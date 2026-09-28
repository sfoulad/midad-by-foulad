add_executable(KeyboardLayoutSetTest
  KeyboardLayoutSetTest.cpp
  ${REPO_ROOT}/src/activities/util/KeyboardLayoutSet.cpp
)

target_include_directories(KeyboardLayoutSetTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/util
)

target_link_libraries(KeyboardLayoutSetTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KeyboardLayoutSetTest)
