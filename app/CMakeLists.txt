add_executable(les main.cpp)

target_link_libraries(les PRIVATE
    les_lib
    cxxopts::cxxopts
)
