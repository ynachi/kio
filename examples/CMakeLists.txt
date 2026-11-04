# ============================================================================
# Examples
# ============================================================================

add_executable(01_tick kio/01_tick.cpp)
target_link_libraries(01_tick PRIVATE uring kio_warnings)
target_compile_definitions(01_tick PRIVATE DAIO_STATS=1)

add_executable(03_http_services kio/03_http_services.cpp)
target_link_libraries(03_http_services PRIVATE uring kio_warnings absl::flags absl::flags_parse)
target_compile_definitions(03_http_services PRIVATE DAIO_STATS=1)

# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        01_tick
        03_http_services
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
