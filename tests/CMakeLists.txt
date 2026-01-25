# Aio tests

# -----------------------------------------------------------------------------
# Unit Tests (fast, no I/O)
# -----------------------------------------------------------------------------

kio_add_test(100_aio_task_tests aio/100_task_tests.cpp
        TAGS aio unit
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(101_aio_pipe_pool_tests aio/101_pipe_pool_tests.cpp
        TAGS aio unit
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(102_io_context aio/102_io_context.cpp
        TAGS aio unit
        LIBS aio GTest::gtest GTest::gtest_main
)

# -----------------------------------------------------------------------------
# Integration Tests (real I/O)
# -----------------------------------------------------------------------------

kio_add_test(200_aio_io_ops_tests aio/200_io_ops_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(201_aio_io_helpers_tests aio/201_io_helpers_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(202_aio_timeout_tests aio/202_timeout_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

kio_add_test(203_aio_net_tests aio/203_net_tests.cpp
        TAGS aio integration
        LIBS aio GTest::gtest GTest::gtest_main
)

