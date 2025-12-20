# ============================================================================
# Demo Helper Function
# ============================================================================
function(add_kio_demo demo_name source_file)
    add_executable(${demo_name} ${source_file})
    target_link_libraries(${demo_name} PRIVATE kio_lib ${ARGN})
endfunction()

# ============================================================================
# Demo Executables
# ============================================================================
add_kio_demo(tcp_demo tcp.cpp)
add_kio_demo(tcp_client_demo tcp_client.cpp)
add_kio_demo(tcp_worker_callback tcp_worker_callback.cpp)
add_kio_demo(tcp_worker_no_callback tcp_worker_no_callback.cpp)
add_kio_demo(tcp_worker_no_callback_v2 tcp_worker_no_callback_v2.cpp)
add_kio_demo(fs_demo fs_demo.cpp)
add_kio_demo(file_demo file_demo.cpp)
add_kio_demo(timer_demo timer.cpp)
add_kio_demo(retry_demo retry01.cpp)
add_kio_demo(kio_logger_demo demo_logging.cpp)
add_kio_demo(kio_baton_demo baton.cpp)
add_kio_demo(ktls_server_demo ktls_server.cpp)
add_kio_demo(ktls_client_demo ktls_client.cpp gflags::gflags)
add_kio_demo(ktls_send_server tls_file_server/file_server.cpp gflags::gflags)
add_kio_demo(ktls_send_client tls_file_server/file_client.cpp gflags::gflags)