add_executable(HttpUrlOriginTest
  HttpUrlOriginTest.cpp
)

target_include_directories(HttpUrlOriginTest PRIVATE
  ${REPO_ROOT}/lib/UrlOrigin
  ${REPO_ROOT}/src/network
)

target_link_libraries(HttpUrlOriginTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpUrlOriginTest)
