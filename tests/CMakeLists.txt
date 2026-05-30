# kio tests

#========= uring
kio_add_test(op_pool_tests uring/op_pool_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(task_tests uring/task_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(mpsc_queue_tests uring/mpsc_queue_test.cpp
        TAGS uring unit queue
        LIBS GTest::gtest GTest::gtest_main
)
if (TARGET mpsc_queue_tests)
    target_include_directories(mpsc_queue_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif ()

kio_add_test(logger_tests uring/logger_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(context_remote_tests uring/context_remote_test.cpp
        TAGS uring integration queue
        LIBS uring GTest::gtest GTest::gtest_main
)

#===============================
# Bitcask tests
#================================
kio_add_test(bitcask_cache_tests bitcask/cache_test.cc
        TAGS bitcask unit cache
        LIBS bitcask GTest::gtest GTest::gtest_main
)

kio_add_test(bitcask_segment_tests bitcask/segment_test.cc
        TAGS bitcask unit segment
        LIBS bitcask GTest::gtest GTest::gtest_main
)