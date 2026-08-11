add_executable(FouladOpdsHooksPureTest
  FouladOpdsHooksPureTest.cpp
  ${REPO_ROOT}/src/FouladOpdsHooksPure.cpp
)

target_include_directories(FouladOpdsHooksPureTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(FouladOpdsHooksPureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FouladOpdsHooksPureTest)
