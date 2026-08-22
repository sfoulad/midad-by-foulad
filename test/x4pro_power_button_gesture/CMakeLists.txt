add_executable(X4ProPowerButtonGestureTest
  X4ProPowerButtonGestureTest.cpp
  ${REPO_ROOT}/src/X4ProPowerButtonGesture.cpp
)

target_include_directories(X4ProPowerButtonGestureTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(X4ProPowerButtonGestureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(X4ProPowerButtonGestureTest)
