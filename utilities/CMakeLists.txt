add_library(kio_utilities INTERFACE)

target_include_directories(kio_utilities INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
kio_target_compile_options(kio_utilities)