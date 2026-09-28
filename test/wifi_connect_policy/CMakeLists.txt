add_executable(WifiConnectPolicyTest
  WifiConnectPolicyTest.cpp
)

target_include_directories(WifiConnectPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(WifiConnectPolicyTest PRIVATE
  crosspoint_test_common

  GTest::gtest_main
)

gtest_discover_tests(WifiConnectPolicyTest)
