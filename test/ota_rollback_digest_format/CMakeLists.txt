add_executable(OtaRollbackDigestFormatTest
  OtaRollbackDigestFormatTest.cpp
  ${REPO_ROOT}/src/OtaRollbackDigestFormat.cpp
)

target_include_directories(OtaRollbackDigestFormatTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OtaRollbackDigestFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaRollbackDigestFormatTest)
