# Aio tests

# -----------------------------------------------------------------------------
# Unit Tests (fast, no I/O)
# -----------------------------------------------------------------------------

kio_add_test(aio_task_tests aio/task_tests.cpp
        TAGS aio unit
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(aio_pipe_pool_tests aio/pipe_pool_tests.cpp
        TAGS aio unit
        LIBS aio GTest::gtest GTest::gtest_main
)

# -----------------------------------------------------------------------------
# Integration Tests (real I/O)
# -----------------------------------------------------------------------------

kio_add_test(aio_io_ops_tests aio/io_ops_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(aio_io_helpers_tests aio/io_helpers_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(aio_timeout_tests aio/timeout_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(aio_net_tests aio/net_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

