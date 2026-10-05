# ============================================================================
# Examples
# ============================================================================

#kio_add_demo(02_echo_server kio/02_echo_server_dispatch.cpp TAGS core LIBS gflags::gflags uring)
#kio_add_sanitizers(02_echo_server)

add_executable(03_http_services kio/03_http_services.cpp)
target_link_libraries(03_http_services PRIVATE uring kio_warnings)
target_compile_definitions(03_http_services PRIVATE DAIO_STATS=1)

# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        #02_echo_server
        03_http_services
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
