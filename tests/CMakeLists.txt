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

kio_add_test(spsc_queue_tests uring/spsc_queue_test.cpp
        TAGS uring unit queue
        LIBS GTest::gtest GTest::gtest_main
)
if (TARGET spsc_queue_tests)
    target_include_directories(spsc_queue_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif ()

kio_add_test(context_remote_tests uring/context_remote_test.cpp
        TAGS uring integration queue
        LIBS uring GTest::gtest GTest::gtest_main
)
