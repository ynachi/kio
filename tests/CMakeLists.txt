# kio tests

kio_add_test(task_tests kio/task_test.cpp
        TAGS uring unit
        LIBS uring GTest::gtest GTest::gtest_main
)

kio_add_test(ready_ring_tests kio/ready_ring_test.cpp
        TAGS unit
)

kio_add_test(run_loop_tests kio/run_loop_test.cpp
        TAGS uring unit
)

kio_add_test(buffer_pool_tests kio/buffer_pool_test.cpp
        TAGS uring unit
)

kio_add_test(baton_semaphore_tests kio/baton_semaphore_test.cpp
        TAGS uring unit
)

kio_add_test(timing_cancel_tests kio/timing_cancel_test.cpp
        TAGS uring unit
)

# Stale v1 tests: target the deleted core/io.h API (IO, run_blocking,
# schedule()). Restore each one as its v2 counterpart lands.
#  - context_remote_tests  (v1: IoContext workers, shared work queues)
#  - shutdown_tests        (v1: grace period + drain_until_quiescent)
#  - io_regression_tests   (v1: ring ownership, queue internals)
#
# kio_add_test(context_remote_tests kio/context_remote_test.cpp
#         TAGS uring integration queue
#         LIBS uring GTest::gtest GTest::gtest_main
# )
#
# kio_add_test(shutdown_tests kio/shutdown_test.cpp
#         TAGS uring integration shutdown
#         LIBS uring GTest::gtest GTest::gtest_main
# )
#
# kio_add_test(io_regression_tests kio/io_regression_test.cpp
#         TAGS uring integration
#         LIBS uring GTest::gtest GTest::gtest_main
# )
