# ============================================================================
# Study labs (study/README.md): one plain executable per lab, built through
# the repo's presets — same warnings, sanitizers, ccache and container
# workflow as the library itself.
#
# Register each lab as you write it (keep them ordered by lab number):
#   kio_add_lab(lab01_ring lab01_ring.cpp)                    # plain C++
#   kio_add_lab(lab04_uring lab04_uring.cpp LINK uring)       # needs kio/io_uring
#
# Labs are programs, not tests: anything that becomes a gtest case belongs
# in tests/ with kio_add_test instead (see study/README.md Stage 8).
# ============================================================================
function(kio_add_lab name source)
    cmake_parse_arguments(ARG "" "" "LINK" ${ARGN})
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE kio_warnings ${ARG_LINK})
endfunction()

# kio_add_lab(lab01_ring lab01_ring.cpp)
