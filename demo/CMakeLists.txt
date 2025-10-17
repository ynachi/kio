add_executable(tcp_demo tcp.cpp)
target_link_libraries(tcp_demo PRIVATE coro_lib)

add_executable(fs_demo fs_demo.cpp)
target_link_libraries(fs_demo PRIVATE coro_lib)