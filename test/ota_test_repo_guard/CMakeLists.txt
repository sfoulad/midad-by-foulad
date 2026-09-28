add_executable(OtaTestRepoGuardTest
  OtaTestRepoGuardTest.cpp
)

target_compile_definitions(OtaTestRepoGuardTest PRIVATE
  REPO_ROOT_DIR="${REPO_ROOT}"
)

target_link_libraries(OtaTestRepoGuardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaTestRepoGuardTest)
