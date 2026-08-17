add_executable(OtaRollbackRecoveryPlanTest
  OtaRollbackRecoveryPlanTest.cpp
  ${REPO_ROOT}/src/OtaRollbackRecoveryPlan.cpp
)

target_include_directories(OtaRollbackRecoveryPlanTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OtaRollbackRecoveryPlanTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaRollbackRecoveryPlanTest)
