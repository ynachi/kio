# Header-only library
add_library(kio_runtime_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )
target_link_libraries(kio_runtime_lib INTERFACE
        PkgConfig::LibUring
        pthread
)
kio_target_compile_options(kio_runtime_lib)

## Single-threaded example
#add_executable(example example.cpp)
#target_link_libraries(example PRIVATE uring_exec)
##kio_add_sanitizers(example)
#
## Multi-threaded example
#add_executable(mt_example mt_example.cpp)
#target_link_libraries(mt_example PRIVATE uring_exec)
##kio_add_sanitizers(mt_example)

kio_add_demo(tcp_demo_v5 tcp_demo.cpp
        TAGS runtime
        LIBS kio_runtime_lib kio_utilities
)


