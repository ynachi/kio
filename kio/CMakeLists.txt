set(CORE_HEADER_FILES
        core/coro.h
        core/uring_awaitable.h
        core/worker.h
        core/worker_pool.h
        core/metrics_collector.h
        include/ds/mpsc_queue.h
        core/sync_wait.h
        core/errors.h
        include/fs.h
        core/async_logger.h
        include/ds/buffer_pool.h
        include/ds/bytes_mut.h
        include/sync/baton.h
        include/sync/event.h
        include/sync/when_any.h

        # metrics
        include/metrics/model.h
        include/metrics/serializer.h
        include/metrics/utils.h
        include/metrics/collector.h
        include/metrics/registry.h
        include/metrics/server.h
)

set(CORE_SRC_FILES
        src/coro.cpp
        core/worker.cpp
        core/worker_pool.cpp
        core/metrics_collector.cpp
        include/net.h
        src/net.cpp
        src/fs.cpp
        src/ds/buffer_pool.cpp
        src/metrics/serializer.cpp
        src/metrics/server.cpp
)

add_library(kio_lib ${CORE_HEADER_FILES} ${CORE_SRC_FILES})
target_link_libraries(kio_lib PUBLIC PkgConfig::LibUring yalantinglibs::yalantinglibs)
target_compile_options(kio_lib PRIVATE -Wall)
target_compile_definitions(kio_lib PRIVATE _GNU_SOURCE)
