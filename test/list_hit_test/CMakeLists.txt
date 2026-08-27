add_executable(ListHitTestTest
  ListHitTestTest.cpp
)

target_include_directories(ListHitTestTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ListHitTestTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListHitTestTest)
