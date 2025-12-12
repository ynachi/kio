add_executable(tcp_demo tcp.cpp)
target_link_libraries(tcp_demo PRIVATE kio_lib)

add_executable(tcp_client_demo tcp_client.cpp)
target_link_libraries(tcp_client_demo PRIVATE kio_lib)

add_executable(tcp_worker_callback tcp_worker_callback.cpp)
target_link_libraries(tcp_worker_callback kio_lib)

add_executable(tcp_worker_no_callback tcp_worker_no_callback.cpp)
target_link_libraries(tcp_worker_no_callback kio_lib)

add_executable(tcp_worker_no_callback_v2 tcp_worker_no_callback_v2.cpp)
target_link_libraries(tcp_worker_no_callback_v2 kio_lib)

add_executable(fs_demo fs_demo.cpp)
target_link_libraries(fs_demo PRIVATE kio_lib)

add_executable(file_demo file_demo.cpp)
target_link_libraries(file_demo PRIVATE kio_lib)

# Timer demo
add_executable(timer_demo timer.cpp)
target_link_libraries(timer_demo PRIVATE kio_lib)

# Retry demo
add_executable(retry_demo retry01.cpp)
target_link_libraries(retry_demo PRIVATE kio_lib)

# Kio logger's demo
add_executable(kio_logger_demo demo_logging.cpp)
target_link_libraries(retry_demo PRIVATE kio_lib)

# Kio baton demo
add_executable(kio_baton_demo baton.cpp)
target_link_libraries(kio_baton_demo PRIVATE kio_lib)

# Kio ktls demo
add_executable(ktls_server_demo ktls_server.cpp)
target_link_libraries(ktls_server_demo PRIVATE kio_lib)

add_executable(ktls_client_demo ktls_client.cpp)
target_link_libraries(ktls_client_demo PRIVATE kio_lib gflags::gflags)

# Tls file server/client
add_executable(ktls_file_server
        tls_file_server/protocol.h
        tls_file_server/file_client.cpp
)
target_link_libraries(ktls_file_server PRIVATE kio_lib gflags::gflags)

add_executable(ktls_file_client
        tls_file_server/protocol.h
        tls_file_server/file_client.cpp
)
target_link_libraries(ktls_file_client PRIVATE kio_lib gflags::gflags)