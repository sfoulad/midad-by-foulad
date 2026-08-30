add_executable(CoverThumbsTest
  CoverThumbsTest.cpp
  ${REPO_ROOT}/src/util/CoverDiagnostics.cpp
  ${REPO_ROOT}/src/util/CoverThumbs.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
)

# stubs/ FIRST so <HalStorage.h> resolves to the stdio-backed host stand-in
# instead of the FreeRTOS/SdFat header, letting the real Bitmap parser build
# here. The same shadowing gives CoverThumbs.cpp its <Arduino.h>, <Logging.h>
# (capturing, so the one-line-per-fault rule can be counted) and
# "util/RollingSdLog.h".
target_include_directories(CoverThumbsTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(CoverThumbsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(CoverThumbsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoverThumbsTest)
