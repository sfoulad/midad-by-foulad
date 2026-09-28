add_executable(FouladCoreEndpointsTest
  FouladCoreEndpointsTest.cpp
)

target_include_directories(FouladCoreEndpointsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(FouladCoreEndpointsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FouladCoreEndpointsTest)
