# minibidi.c is plain C; the top-level project only enables CXX, so this test is the
# first to need a C compiler.
enable_language(C)

add_executable(BidiUtilsTest
  BidiUtilsTest.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BidiUtilsTest PRIVATE
  stubs
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(BidiUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BidiUtilsTest)
