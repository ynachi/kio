# Test executables
add_executable(sync_wait_test sync_wait_test.cpp)
add_executable(lock_free_free_queue ds/mpsc_queue_test.cpp)

target_link_libraries(sync_wait_test
        PRIVATE
        core_lib
        GTest::gtest
        GTest::gtest_main
)

target_link_libraries(lock_free_free_queue
        PRIVATE
        core_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(sync_wait_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(lock_free_free_queue
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Discover and register tests
gtest_discover_tests(sync_wait_test lock_free_free_queue)
