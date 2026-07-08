add_executable(ArabicShaperTest
  ArabicShaperTest.cpp
  ${REPO_ROOT}/lib/ArabicShaper/ArabicShaper.cpp
  ${REPO_ROOT}/lib/ArabicShaper/ArabicCharacter.cpp
)

target_include_directories(ArabicShaperTest PRIVATE
  ${REPO_ROOT}/lib/ArabicShaper
)

target_link_libraries(ArabicShaperTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ArabicShaperTest)
