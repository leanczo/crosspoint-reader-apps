add_executable(ChessEngineTest
  ChessEngineTest.cpp
  ${REPO_ROOT}/lib/Chess/ChessEngine.cpp
)

target_include_directories(ChessEngineTest PRIVATE
  ${REPO_ROOT}/lib/Chess
)

target_link_libraries(ChessEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChessEngineTest)
