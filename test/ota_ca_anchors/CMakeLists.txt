add_executable(OtaCaAnchorsTest
  OtaCaAnchorsTest.cpp
)

target_include_directories(OtaCaAnchorsTest PRIVATE
  ${REPO_ROOT}/src
)

# The recorded live chains (leaf + served intermediates per host) the anchors
# must keep verifying; see OtaCaAnchorsTest.cpp for capture provenance.
target_compile_definitions(OtaCaAnchorsTest PRIVATE
  OTA_CA_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(OtaCaAnchorsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaCaAnchorsTest)
