# ============================================================================
# Examples
# ============================================================================

kio_add_demo(00_logger aio/00_logger.cpp TAGS core LIBS aio)
kio_add_sanitizers(00_logger)

kio_add_demo(06_worker_wackup aio/06_worker_wackup.cpp TAGS core LIBS aio)
kio_add_sanitizers(06_worker_wackup)

kio_add_demo(10_integrated_file_server aio/10_integrated_file_server.cpp TAGS core LIBS aio)
kio_add_sanitizers(10_integrated_file_server)

kio_add_demo(200_hello_sleep aio/200_hello_sleep.cpp TAGS core LIBS aio)
kio_add_sanitizers(200_hello_sleep)

kio_add_demo(201_aio_timers aio/201_timers.cpp TAGS core LIBS aio)
kio_add_sanitizers(201_aio_timers)

kio_add_demo(202_aio_timers aio/202_echo_server.cpp TAGS core LIBS aio)
kio_add_sanitizers(202_aio_timers)

kio_add_demo(203_file_copy aio/203_file_copy.cpp TAGS core LIBS gflags::gflags aio)
kio_add_sanitizers(203_file_copy)

kio_add_demo(204_timeout aio/204_timeouts.cpp TAGS core LIBS aio)
kio_add_sanitizers(204_timeout)

kio_add_demo(205_task_group aio/205_task_group.cpp TAGS core LIBS aio)
kio_add_sanitizers(205_task_group)

kio_add_demo(206_graceful_shutdown aio/206_graceful_shutdown.cpp TAGS core LIBS aio)
kio_add_sanitizers(206_graceful_shutdown)

kio_add_demo(207_blocking_offload aio/207_blocking_offload.cpp TAGS core LIBS aio)
kio_add_sanitizers(207_blocking_offload)

kio_add_demo(208_static_file_server aio/208_static_file_server.cpp TAGS core LIBS aio)
kio_add_sanitizers(208_static_file_server)

kio_add_demo(209_multi_worker aio/209_multi_worker.cpp TAGS core LIBS aio)
kio_add_sanitizers(209_multi_worker)

kio_add_demo(210_chat_server aio/210_chat_server.cpp TAGS core LIBS aio)
kio_add_sanitizers(210_chat_server)

kio_add_demo(211_dns_resolver aio/211_dns_resolve.cpp TAGS core LIBS aio)
kio_add_sanitizers(211_dns_resolver)

kio_add_demo(212_hello_http_worker aio/212_hello_http_worker.cpp TAGS core LIBS aio)
kio_add_sanitizers(212_hello_http_worker)

kio_add_demo(213_hello_http_worker_single_acceptor aio/213_hello_http_single_acceptor.cpp TAGS core LIBS aio)
kio_add_sanitizers(213_hello_http_worker_single_acceptor)


# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        00_logger
        200_hello_sleep
        201_aio_timers
        202_aio_timers
        203_file_copy
        204_timeout
        205_task_group
        206_graceful_shutdown
        207_blocking_offload
        208_static_file_server
        209_multi_worker
        210_chat_server
        211_dns_resolver
        212_hello_http_worker
        213_hello_http_worker_single_acceptor
        06_worker_wackup
        10_integrated_file_server
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
