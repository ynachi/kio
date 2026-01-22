# Aio tests

kio_add_test(aio_tests aio/aio_tests.cpp
        TAGS aio
        LIBS aio GTest::gtest GTest::gtest_main
)


#kio_add_test(aio_blocking_pool_tests aio/blocking_pool_test.cpp
#        TAGS aio
#        LIBS aio GTest::gtest GTest::gtest_main
#)
#kio_add_sanitizers(aio_blocking_pool_tests)
#
#kio_add_test(aio_task_group_tests aio/task_group_tests.cpp
#        TAGS aio
#        LIBS aio GTest::gtest GTest::gtest_main
#)
#kio_add_sanitizers(aio_task_group_tests)