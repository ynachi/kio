add_library(cpp_library INTERFACE)

target_include_directories(cpp_library INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(cpp_library INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/xxhash
)