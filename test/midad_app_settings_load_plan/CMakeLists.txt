add_executable(MidadAppSettingsLoadPlanTest
  MidadAppSettingsLoadPlanTest.cpp
  ${REPO_ROOT}/src/MidadAppSettingsLoadPlan.cpp
)

target_include_directories(MidadAppSettingsLoadPlanTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(MidadAppSettingsLoadPlanTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MidadAppSettingsLoadPlanTest)
