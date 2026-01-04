# ============================================================================
# Core Runtime Headers
# ============================================================================
set(core_runtime_headers
        core/coro.h
        core/worker.h
        core/worker_pool.h
        core/metrics_collector.h
        core/errors.h
        core/async_logger.h
        core/bytes_mut.h
        core/safe_completion.h
        core/chunked_buffer.h
)

set(core_runtime_sources
        core/worker.cpp
        core/worker_pool.cpp
        core/metrics_collector.cpp
        core/chunked_buffer.cpp
)

# ============================================================================
# Synchronization Primitives
# ============================================================================
set(sync_headers
        sync/mpsc_queue.h
        sync/sync_wait.h
        sync/baton.h
)

# ============================================================================
# Filesystem
# ============================================================================
set(fs_headers
        fs/fs.h
)

set(fs_sources
        fs/fs.cpp
)

# ============================================================================
# Networking
# ============================================================================
set(net_headers
        net/net.h
        net/socket.h
)

set(net_sources
        net/net.cpp
)

# ============================================================================
# TLS Support
# ============================================================================
set(tls_headers
        tls/context.h
        tls/stream.h
        tls/listener.h
)

set(tls_sources
        tls/context.cpp
        tls/stream.cpp
        tls/listener.cpp
)

# ============================================================================
# Metrics
# ============================================================================
set(metrics_headers
        metrics/model.h
        metrics/serializer.h
        metrics/utils.h
        metrics/collector.h
        metrics/registry.h
        metrics/server.h
)

set(metrics_sources
        metrics/serializer.cpp
        metrics/server.cpp
)

# ============================================================================
# Protocol Support - RESP
# ============================================================================
set(resp_headers
        resp/parser.h
        resp/writer.h
)

set(resp_sources
        resp/parser.cpp
)

# ============================================================================
# KIO Library Target
# ============================================================================
add_library(kio_lib
        ${core_runtime_headers}
        ${core_runtime_sources}
        ${sync_headers}
        ${fs_headers}
        ${fs_sources}
        ${net_headers}
        ${net_sources}
        ${tls_headers}
        ${tls_sources}
        ${metrics_headers}
        ${metrics_sources}
        ${resp_headers}
        ${resp_sources}
)

target_link_libraries(kio_lib PRIVATE
        PkgConfig::LibUring
        yalantinglibs::yalantinglibs
        OpenSSL::SSL
        OpenSSL::Crypto
        crc32c
)

kio_target_compile_options(kio_lib)
target_compile_definitions(kio_lib PRIVATE _GNU_SOURCE)