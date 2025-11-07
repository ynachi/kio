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

