add_executable(BleDeviceInfoPayloadTest
  BleDeviceInfoPayloadTest.cpp
  ${REPO_ROOT}/src/BleDeviceInfoPayload.cpp
)

target_include_directories(BleDeviceInfoPayloadTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(BleDeviceInfoPayloadTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(BleDeviceInfoPayloadTest)
