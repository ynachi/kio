# ============================================================================
# Test Helper Function
# ============================================================================
function(add_kio_root_test test_name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    if (NOT ARG_SOURCES)
        set(ARG_SOURCES ${test_name}.cpp)
    endif ()

    add_executable(${test_name} ${ARG_SOURCES})

    target_link_libraries(${test_name} PRIVATE
            kio_lib
            GTest::gtest
            GTest::gtest_main
    )

    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    kio_target_compile_options(${test_name})

    gtest_discover_tests(${test_name})
endfunction()

# ============================================================================
# Define Tests
# ============================================================================
add_kio_root_test(sync_wait_test)
add_kio_root_test(lock_free_free_queue SOURCES ds/mpsc_queue_test.cpp)
add_kio_root_test(kio_worker_test SOURCES worker_test.cpp)
add_kio_root_test(filemanager_test)
add_kio_root_test(kio_logger_test)
add_kio_root_test(buffer_pool_test SOURCES ds/buffer_pool_test.cpp)

add_kio_root_test(sync_tests SOURCES
        sync/baton_test.cpp
)

add_kio_root_test(bytes_mut_tests SOURCES ds/bytes_mut_test.cpp)
