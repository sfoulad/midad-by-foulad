add_executable(ReadingHeatmapLayoutTest
  ReadingHeatmapLayoutTest.cpp
)

target_include_directories(ReadingHeatmapLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReadingHeatmapLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingHeatmapLayoutTest)
