add_executable(run_tests
               compressors_test.cpp)

target_link_libraries(run_tests PRIVATE
    les_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(run_tests)