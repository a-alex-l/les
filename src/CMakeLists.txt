set(LIB_SOURCES
    compression_classifier.cpp
    fse_compressor.cpp
    huffman_compressor.cpp
    lz_compressor.cpp
    chunker.cpp
    common/entropy.cpp
)

add_library(les_lib ${LIB_SOURCES})

target_link_libraries(les_lib PRIVATE Threads::Threads)

target_include_directories(les_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)
