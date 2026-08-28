add_executable(AppsGridLayoutTest
  AppsGridLayoutTest.cpp
)

target_include_directories(AppsGridLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(AppsGridLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AppsGridLayoutTest)
