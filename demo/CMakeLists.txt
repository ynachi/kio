# ============================================================================
# Demo Executables
# ============================================================================
kio_add_demo(tcp_demo tcp.cpp TAGS core LIBS kio_lib)
kio_add_demo(tcp_client_demo tcp_client.cpp TAGS core LIBS kio_lib)
kio_add_demo(tcp_worker_callback tcp_worker_callback.cpp TAGS core LIBS kio_lib)
kio_add_demo(tcp_worker_no_callback tcp_worker_no_callback.cpp TAGS core LIBS kio_lib)
kio_add_demo(tcp_worker_no_callback_v2 tcp_worker_no_callback_v2.cpp TAGS core LIBS kio_lib)
kio_add_demo(fs_demo fs_demo.cpp TAGS core LIBS kio_lib)
kio_add_demo(file_demo file_demo.cpp TAGS core LIBS kio_lib)
kio_add_demo(timer_demo timer.cpp TAGS core LIBS kio_lib)
kio_add_demo(retry_demo retry01.cpp TAGS core LIBS kio_lib)
kio_add_demo(kio_logger_demo demo_logging.cpp TAGS core LIBS kio_lib)
kio_add_demo(kio_baton_demo baton.cpp TAGS core LIBS kio_lib)
kio_add_demo(ktls_server_demo ktls_server.cpp TAGS core LIBS kio_lib)
kio_add_demo(ktls_client_demo ktls_client.cpp TAGS core LIBS kio_lib gflags::gflags)
kio_add_demo(ktls_send_server tls_file_server/file_server.cpp TAGS core LIBS kio_lib gflags::gflags)
kio_add_demo(ktls_send_client tls_file_server/file_client.cpp TAGS core LIBS kio_lib gflags::gflags)


kio_add_demo(tcp_demo_next tcp_next.cpp)