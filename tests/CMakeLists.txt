# Test executables
add_executable(sync_wait_test sync_wait_test.cpp)
add_executable(lock_free_free_queue ds/mpsc_queue_test.cpp)

target_link_libraries(sync_wait_test
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_link_libraries(lock_free_free_queue
        PRIVATE
        kio_lib
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

# worker tests
add_executable(kio_worker_test worker_test.cpp)

target_link_libraries(kio_worker_test
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(kio_worker_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(kio_worker_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# file manager tests
add_executable(filemanager_test filemanager_test.cpp)

target_link_libraries(filemanager_test
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(filemanager_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(filemanager_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)


# kio logger tests
add_executable(kio_logger_test kio_logger_test.cpp)

target_link_libraries(kio_logger_test
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(kio_logger_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(kio_logger_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# buffer pool tests
add_executable(buffer_pool_test ds/buffer_pool_test.cpp)

target_link_libraries(buffer_pool_test
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(buffer_pool_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# sync tests
add_executable(sync_tests
        sync/baton_test.cpp
        sync/event_test.cpp
        sync/when_any_test.cpp
)

target_link_libraries(sync_tests
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(sync_tests
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# bytes_mut tests
add_executable(bytes_mut_tests
        ds/bytes_mut_test.cpp
)

target_link_libraries(bytes_mut_tests
        PRIVATE
        kio_lib
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(bytes_mut_tests
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)


gtest_discover_tests(sync_wait_test)
gtest_discover_tests(lock_free_free_queue)
gtest_discover_tests(kio_worker_test)
gtest_discover_tests(filemanager_test)
gtest_discover_tests(kio_logger_test)
gtest_discover_tests(buffer_pool_test)
gtest_discover_tests(sync_tests)
gtest_discover_tests(bytes_mut_tests)


