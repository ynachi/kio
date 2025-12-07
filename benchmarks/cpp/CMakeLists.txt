add_executable(net_perf net_perf.cpp)
target_link_libraries(net_perf PRIVATE kio_lib gflags::gflags)

add_executable(net_perf_photon net_perf_photon.cpp)
target_link_libraries(net_perf_photon PRIVATE kio_lib gflags::gflags photon_static)