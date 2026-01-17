add_library(third_party INTERFACE)

target_include_directories(third_party INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(third_party INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/xxhash
)