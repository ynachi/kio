add_executable(tcp_demo tcp.cpp)
target_link_libraries(tcp_demo PRIVATE core_lib)

add_executable(tcp_worker_callback tcp_worker_callback.cpp)
target_link_libraries(tcp_worker_callback core_lib)

add_executable(tcp_worker_no_callback tcp_worker_no_callback.cpp)
target_link_libraries(tcp_worker_no_callback core_lib)

add_executable(tcp_worker_no_callback_v2 tcp_worker_no_callback_v2.cpp)
target_link_libraries(tcp_worker_no_callback_v2 core_lib)

add_executable(fs_demo fs_demo.cpp)
target_link_libraries(fs_demo PRIVATE core_lib)

add_executable(file_demo file_demo.cpp)
target_link_libraries(file_demo PRIVATE core_lib)

# Timer demo
add_executable(timer_demo timer.cpp)
target_link_libraries(timer_demo PRIVATE core_lib)

# Retry demo
add_executable(retry_demo retry01.cpp)
target_link_libraries(retry_demo PRIVATE core_lib)

# Kio logger's demo
add_executable(kio_logger_demo demo_logging.cpp)
target_link_libraries(retry_demo PRIVATE core_lib)
