function(add_sanitizers target_name)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -fsanitize=thread -g)
        target_link_options(${target_name} PRIVATE -fsanitize=thread)
    endif ()
endfunction()

# The HTTP benchmark server `make bench` runs. Deliberately has no external
# dependency beyond kio itself, so the release preset can build it without
# fetching anything.
add_executable(kio_http_bench kio_http_bench.cpp)
# NB: the CMake target is still `uring`. The uring->kio rename moved the
# directory and the public headers but never renamed the target itself.
target_link_libraries(kio_http_bench PRIVATE uring)
add_sanitizers(kio_http_bench)

# kio vs Photon. Photon and gflags are NOT vendored or fetched by this project,
# so this target is off by default: leaving it in the unconditional path made
# the whole benchmark directory fail to configure for anyone who had never
# enabled KIO_BUILD_BENCH before. Turn it on only in an environment where
# photon_static and gflags::gflags already exist.
option(KIO_BUILD_PHOTON_BENCH "Build the kio-vs-Photon network benchmark" OFF)
if (KIO_BUILD_PHOTON_BENCH)
    add_executable(net_perf_photon net_perf_photon.cpp)
    target_link_libraries(net_perf_photon PRIVATE gflags::gflags photon_static)
    add_sanitizers(net_perf_photon)
endif ()