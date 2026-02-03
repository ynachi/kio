# ============================================================================
# Examples
# ============================================================================

kio_add_demo(01_core_concepts aio/01_core_concepts.cpp TAGS core LIBS kio)
kio_add_sanitizers(01_core_concepts)

kio_add_demo(02_echo_server aio/02_echo_server.cpp TAGS core LIBS gflags::gflags kio)
kio_add_sanitizers(02_echo_server)

kio_add_demo(03_http_services aio/03_http_services.cpp TAGS core LIBS gflags::gflags kio)
kio_add_sanitizers(03_http_services)
target_compile_definitions(03_http_services PRIVATE DAIO_STATS=1)

kio_add_demo(211_dns_resolver aio/211_dns_resolve.cpp TAGS core LIBS kio)
kio_add_sanitizers(211_dns_resolver)


kio_add_demo(215_tls_echo_server aio/215_tls_echo_server.cpp TAGS core LIBS gflags::gflags kio)
kio_add_sanitizers(215_tls_echo_server)


# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        01_core_concepts
        02_echo_server
        03_http_services
        211_dns_resolver
        215_tls_echo_server
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
