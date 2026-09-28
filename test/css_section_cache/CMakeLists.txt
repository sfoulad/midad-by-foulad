add_executable(CssSectionCacheTest
  CssSectionCacheTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(CssSectionCacheTest PRIVATE
  ${REPO_ROOT}/test/css_parser/stubs
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(CssSectionCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssSectionCacheTest)
