pkg_check_modules(LIBURING REQUIRED liburing)

set(CORE_HEADER_FILES
        include/coro.h
        include/io/uring_awaitable.h
        include/io/worker.h
        include/io/worker_pool.h
        include/io/metrics.h
        include/ds/mpsc_queue.h
        include/sync_wait.h
        include/errors.h
        include/fs.h
        include/async_logger.h
        include/ds/buffer_pool.h
        include/sync/baton.h
        include/sync/event.h

        # metrics
        include/metrics/model.h
        include/metrics/serializer.h
        include/metrics/utils.h
        include/metrics/collector.h
        include/metrics/registry.h
)

set(CORE_SRC_FILES
        src/coro.cpp
        src/io/worker.cpp
        src/io/worker_pool.cpp
        src/io/metrics.cpp
        include/net.h
        src/net.cpp
        src/fs.cpp
        src/ds/buffer_pool.cpp
        src/metrics/serializer.cpp
)

add_library(kio_lib ${CORE_HEADER_FILES} ${CORE_SRC_FILES})
target_link_libraries(kio_lib PUBLIC ${LIBURING_LIBRARIES} yalantinglibs::yalantinglibs)
target_compile_options(kio_lib PRIVATE -Wall)
