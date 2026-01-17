# Header-only library
add_library(kio_runtime_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kio_runtime_lib INTERFACE
        PkgConfig::LibUring
        pthread
)
kio_target_compile_options(kio_runtime_lib)

kio_add_demo(tcp_demo_v5 demo/tcp_demo.cpp
        TAGS runtime
        LIBS kio_runtime_lib kio_utilities
)

kio_add_demo(runtime_example demo/example.cpp
        TAGS runtime
        LIBS kio_runtime_lib kio_utilities
)

kio_add_demo(runtime_mt_example demo/mt_example.cpp
        TAGS runtime
        LIBS kio_runtime_lib kio_utilities
)
kio_add_sanitizers(runtime_mt_example)

