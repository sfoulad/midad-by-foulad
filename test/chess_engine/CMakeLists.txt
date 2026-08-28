add_executable(ChessEngineTest
  ChessEngineTest.cpp
  ${REPO_ROOT}/lib/ChessEngine/ChessBoard.cpp
  ${REPO_ROOT}/lib/ChessEngine/ChessSearch.cpp
  ${REPO_ROOT}/lib/ChessEngine/ChessGame.cpp
)

target_include_directories(ChessEngineTest PRIVATE
  ${REPO_ROOT}/lib/ChessEngine
  ${REPO_ROOT}/src
)

target_link_libraries(ChessEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChessEngineTest)
