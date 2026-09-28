add_executable(HttpUrlPartsTest
  HttpUrlPartsTest.cpp
)

target_include_directories(HttpUrlPartsTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(HttpUrlPartsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpUrlPartsTest)
