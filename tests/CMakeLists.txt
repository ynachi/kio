# kio tests

#========= uring

kio_add_test(task_tests uring/task_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(logger_tests uring/logger_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(context_remote_tests uring/context_remote_test.cpp
        TAGS uring integration queue
        LIBS uring GTest::gtest GTest::gtest_main
)
