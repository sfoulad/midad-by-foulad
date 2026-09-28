add_executable(X3MemoryBudgetTest
  X3MemoryBudgetTest.cpp
)

target_include_directories(X3MemoryBudgetTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(X3MemoryBudgetTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(X3MemoryBudgetTest)
