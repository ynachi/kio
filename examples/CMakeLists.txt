# ============================================================================
# Examples
# ============================================================================

kio_add_demo(01_tracing kio/01_tracing.cpp TAGS core LIBS uring)
kio_add_sanitizers(01_tracing)

kio_add_demo(02_echo_server kio/02_echo_server_dispatch.cpp TAGS core LIBS gflags::gflags uring)
kio_add_sanitizers(02_echo_server)

kio_add_demo(03_http_services kio/03_http_services.cpp TAGS core LIBS gflags::gflags uring)
kio_add_sanitizers(03_http_services)
target_compile_definitions(03_http_services PRIVATE DAIO_STATS=1)

kio_add_demo(04_combinators kio/04_combinators.cpp TAGS core LIBS uring)
kio_add_sanitizers(04_combinators)


# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        01_tracing
        02_echo_server
        03_http_services
        04_combinators
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
