pkg_check_modules(LIBURING REQUIRED liburing)

set(CORE_HEADER_FILES
        include/coro.h
        include/io/uring_awaitable.h
        include/io/worker.h
        include/io/worker_pool.h
        include/ds/mpsc_queue.h
        include/sync_wait.h
        include/errors.h
        include/fs.h
        include/async_logger.h
)

set(CORE_SRC_FILES
        src/coro.cpp
        src/io/worker.cpp
        src/io/worker_pool.cpp
        include/net.h
        src/net.cpp
        src/fs.cpp
)

add_library(core_lib ${CORE_HEADER_FILES} ${CORE_SRC_FILES})
target_link_libraries(core_lib PUBLIC ${LIBURING_LIBRARIES})
target_compile_options(core_lib PRIVATE -Wall)
