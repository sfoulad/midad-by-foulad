add_executable(LineHeightMathTest
  LineHeightMathTest.cpp
)

target_link_libraries(LineHeightMathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LineHeightMathTest)
