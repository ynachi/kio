# kio tests

# -----------------------------------------------------------------------------
# Unit Tests (fast, no I/O)
# -----------------------------------------------------------------------------

kio_add_test(101_kio_pipe_pool_tests kio/101_pipe_pool_tests.cpp
        TAGS kio unit
        LIBS kio GTest::gtest GTest::gtest_main
)

kio_add_test(102_io_context kio/102_io_context.cpp
        TAGS kio unit
        LIBS kio GTest::gtest GTest::gtest_main
)

# -----------------------------------------------------------------------------
# Integration Tests (real I/O)
# -----------------------------------------------------------------------------

kio_add_test(200_kio_io_ops_tests kio/200_io_ops_tests.cpp
        TAGS kio integration
        LIBS kio GTest::gtest GTest::gtest_main
)

kio_add_test(201_kio_io_helpers_tests kio/201_io_helpers_tests.cpp
        TAGS kio integration
        LIBS kio GTest::gtest GTest::gtest_main
)

kio_add_test(202_kio_timeout_tests kio/202_timeout_tests.cpp
        TAGS kio integration
        LIBS kio GTest::gtest GTest::gtest_main
)

kio_add_test(203_kio_net_tests kio/203_net_tests.cpp
        TAGS kio integration
        LIBS kio GTest::gtest GTest::gtest_main
)

kio_add_test(301_http_tests kio/301_http_test.cpp
        TAGS kio http unit
        LIBS kio GTest::gtest GTest::gtest_main
)

kio_add_test(401_io_buffer kio/401_io_buffer_tests.cpp
        TAGS kio buffer unit
        LIBS kio GTest::gtest GTest::gtest_main
)


kio_add_test(501_resp_parser kio/resp/501_resp_parser.cpp
        TAGS kio resp unit
        LIBS kio GTest::gtest GTest::gtest_main
)
kio_add_test(502_resp_writer kio/resp/502_resp_writer.cpp
        TAGS kio resp unit
        LIBS kio GTest::gtest GTest::gtest_main
)

#========= uring
kio_add_test(op_pool_tests uring/op_pool_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)