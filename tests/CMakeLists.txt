# kio tests

#========= uring
kio_add_test(op_pool_tests uring/op_pool_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)