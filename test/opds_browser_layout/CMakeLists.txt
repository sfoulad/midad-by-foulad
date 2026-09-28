add_executable(OpdsBrowserLayoutTest
  OpdsBrowserLayoutTest.cpp
)

target_include_directories(OpdsBrowserLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OpdsBrowserLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsBrowserLayoutTest)
