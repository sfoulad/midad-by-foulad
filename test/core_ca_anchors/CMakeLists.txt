add_executable(CoreCaAnchorsTest
  CoreCaAnchorsTest.cpp
)

target_include_directories(CoreCaAnchorsTest PRIVATE
  ${REPO_ROOT}/src
)

# The recorded live chain (leaf + served intermediates) plus the negative
# fixtures; see CoreCaAnchorsTest.cpp for capture provenance.
target_compile_definitions(CoreCaAnchorsTest PRIVATE
  CORE_CA_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(CoreCaAnchorsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoreCaAnchorsTest)
