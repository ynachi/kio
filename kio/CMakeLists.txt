set(CORE_HEADER_FILES
        core/coro.h
        core/uring_awaitable.h
        core/worker.h
        core/worker_pool.h
        core/metrics_collector.h
        sync/mpsc_queue.h
        sync/sync_wait.h
        core/errors.h
        fs/fs.h
        core/async_logger.h
        core/bytes_mut.h
        sync/baton.h
        sync/event.h
        sync/when_any.h

        # metrics
        metrics/model.h
        metrics/serializer.h
        metrics/utils.h
        metrics/collector.h
        metrics/registry.h
        metrics/server.h
)

set(TLS_HEADER_FILES
        tls/context.h
        tls/stream.h
        tls/listener.h
)

set(TLS_SRC_FILES
        tls/context.cpp
        tls/stream.cpp
        tls/listener.cpp
)

set(CORE_SRC_FILES
        core/worker.cpp
        core/worker_pool.cpp
        core/metrics_collector.cpp
        net/net.h
        net/socket.h
        net/net.cpp
        fs/fs.cpp
        metrics/serializer.cpp
        metrics/server.cpp
)

add_library(kio_lib
        ${CORE_HEADER_FILES}
        ${CORE_SRC_FILES}
        ${TLS_HEADER_FILES}
        ${TLS_SRC_FILES}
)

target_link_libraries(kio_lib PRIVATE
        PkgConfig::LibUring
        yalantinglibs::yalantinglibs
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)
target_compile_options(kio_lib PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-gnu-statement-expression
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3>
)
target_compile_definitions(kio_lib PRIVATE _GNU_SOURCE)

