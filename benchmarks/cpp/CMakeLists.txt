#add_executable(net_perf net_perf.cpp)
#target_link_libraries(net_perf PRIVATE kio gflags::gflags)

add_executable(net_perf_photon net_perf_photon.cpp)
target_link_libraries(net_perf_photon PRIVATE gflags::gflags photon_static)

add_executable(storage_perf_photon storage_perf_photon.cpp)
target_link_libraries(storage_perf_photon PRIVATE gflags::gflags photon_static)

add_executable(storage_perf_kio storage_perf_kio.cpp)
target_link_libraries(storage_perf_kio PRIVATE gflags::gflags kio)
target_include_directories(storage_perf_kio PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Force sanitizers for storage_perf_kio to verify correctness
if (ENABLE_ASAN)
    target_compile_options(storage_perf_kio PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(storage_perf_kio PRIVATE -fsanitize=address)
    message(STATUS "ASan enabled for storage_perf_kio")
endif()

if (ENABLE_TSAN)
    target_compile_options(storage_perf_kio PRIVATE -fsanitize=thread -g)
    target_link_options(storage_perf_kio PRIVATE -fsanitize=thread)
    message(STATUS "TSan enabled for storage_perf_kio")
endif()
