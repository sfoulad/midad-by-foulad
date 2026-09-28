add_executable(KOReaderServerOriginTest
  KOReaderServerOriginTest.cpp
)

target_include_directories(KOReaderServerOriginTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderServerOriginTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderServerOriginTest)
