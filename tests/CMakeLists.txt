# kio tests

#========= uring

kio_add_test(task_tests kio/task_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(logger_tests kio/logger_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(context_remote_tests kio/context_remote_test.cpp
        TAGS uring integration queue
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(shutdown_tests kio/shutdown_test.cpp
        TAGS uring integration shutdown
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(io_regression_tests kio/io_regression_test.cpp
        TAGS uring integration
        LIBS uring GTest::gtest GTest::gtest_main
)
