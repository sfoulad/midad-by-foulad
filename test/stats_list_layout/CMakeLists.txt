add_executable(StatsListLayoutTest
  StatsListLayoutTest.cpp
)

target_include_directories(StatsListLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(StatsListLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StatsListLayoutTest)
