add_executable(FirmwareUpdatePolicyTest
  FirmwareUpdatePolicyTest.cpp
  OtaScreenActionsTest.cpp
)

target_include_directories(FirmwareUpdatePolicyTest PRIVATE
  ${REPO_ROOT}/src
)

# The running build's version, as OtaUpdater::isVersionNewer's single-argument
# form sees it on the device that reproduced the re-offer defect.
target_compile_definitions(FirmwareUpdatePolicyTest PRIVATE
  CROSSPOINT_VERSION="1.8.57-rc+254abcd"
)

target_link_libraries(FirmwareUpdatePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareUpdatePolicyTest)
