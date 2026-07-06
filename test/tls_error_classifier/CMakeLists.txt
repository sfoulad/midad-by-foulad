add_executable(TlsErrorClassifierTest
  TlsErrorClassifierTest.cpp
  ${REPO_ROOT}/lib/TlsErrorClassifier/TlsErrorClassifier.cpp
)

target_include_directories(TlsErrorClassifierTest PRIVATE
  ${REPO_ROOT}/lib/TlsErrorClassifier
)

target_link_libraries(TlsErrorClassifierTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TlsErrorClassifierTest)
