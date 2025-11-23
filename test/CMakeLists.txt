
cmake_minimum_required(VERSION 3.14)
project(les CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /Oi /Ot)
else()
    add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

add_executable(run_tests
               compressors_test.cpp)

target_link_libraries(run_tests PRIVATE
    les_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(run_tests)