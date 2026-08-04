add_executable(DateMathTest
  DateMathTest.cpp
  ${REPO_ROOT}/lib/DateMath/DateMath.cpp
)

target_include_directories(DateMathTest PRIVATE
  ${REPO_ROOT}/lib/DateMath
)

target_link_libraries(DateMathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DateMathTest)
