# ============================================================================
# Demo Executables
# ============================================================================
kio_add_demo(00_logger aio/00_logger.cpp TAGS core LIBS aio)
kio_add_sanitizers(00_logger)

kio_add_demo(01_aio_timers aio/01_timers.cpp TAGS core LIBS aio)
kio_add_sanitizers(01_aio_timers)

#kio_add_demo(02_work_queue aio/02_work_queue.cpp TAGS core LIBS aio)
#kio_add_sanitizers(02_work_queue)
#
#kio_add_demo(03_m_server_gemini aio/03_m_server_gemini.cpp TAGS core LIBS aio)
#kio_add_sanitizers(03_m_server_gemini)
#
#kio_add_demo(04_dns_resolve aio/04_dns_resolve.cpp TAGS core LIBS aio)
#kio_add_sanitizers(04_dns_resolve)
#
#kio_add_demo(05_file_copy aio/05_file_copy.cpp TAGS core LIBS aio)
#kio_add_sanitizers(05_file_copy)
#
#kio_add_demo(06_worker_wackup aio/06_worker_wackup.cpp TAGS core LIBS aio)
#kio_add_sanitizers(06_worker_wackup)
#
#kio_add_demo(07_signal_shutdown aio/07_signal_shutdown.cpp TAGS core LIBS aio)
#kio_add_sanitizers(07_signal_shutdown)
#
#kio_add_demo(08_hello_http aio/08_hello_http.cpp TAGS core LIBS aio)
#kio_add_sanitizers(08_hello_http)



# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        00_logger
        01_aio_timers
#        02_echo_server
#        03_m_server_gemini
#        04_dns_resolve
#        05_file_copy
#        06_worker_wackup
#        07_signal_shutdown
#        08_hello_http
)
message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
