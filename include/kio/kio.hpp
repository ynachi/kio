#pragma once

/**
 * Minimal async I/O library built on io_uring and C++23 coroutines
 *
 * Design:
 * - Thread-per-core model: one io_context per thread.
 * - Single Issuer: Uses IORING_SETUP_SINGLE_ISSUER for maximum performance.
 * Only the creating thread may submit operations (Track/GetSqe).
 * - Coroutine handle stored in user_data for direct resume.
 * - Intrusive tracking of pending operations for safe cancellation/shutdown.
 * - Caller owns buffers; library does not allocate.
 *
 * Thread Safety:
 * - IoContext is NOT thread-safe for submission (Track, GetSqe, Step).
 * - Notify() IS thread-safe and can be called from any thread to wake the loop.
 * - GetMetrics() IS thread-safe (uses relaxed atomics).
 *
 * Safety:
 * Operation lifetimes are tracked via an intrusive linked list. If a coroutine
 * frame is destroyed while an I/O operation is still pending (kernel hasn't
 * completed it), the program will terminate rather than risk silent memory
 * corruption. This is detection, not prevention.
 *
 * !! IMPORTANT !!
 * Operations are embedded in coroutine frames. If you destroy a task while its
 * coroutine is suspended on I/O, you WILL hit std::terminate(). This is
 * intentional — the alternative is silent memory corruption when the kernel
 * writes to freed memory.
 *
 * Running a task example
 * ctx.RunUntilDone(HandleClient(ctx, fd));
 *
 * group.Spawn(HandleClient(ctx, fd));
 *
 * To safely cancel work:
 * 1. Use timeouts (.with_timeout()) so operations eventually complete.
 * 2. Let io_context::cancel_all_pending() drain on destruction.
 * 3. Use TaskGroup to manage parallel task lifetimes and wait for completion.
 *
 * Observability:
 * Define AIO_STATS=1 to enable internal metrics counters (submitted, completed,
 * inflight, etc.). Accessible via ctx.GetMetrics().
 *
 * Requirements:
 * - Linux kernel >= 6.0
 * - liburing
 * - C++23
 **/

#include "resp/parser.h"
#include "resp/writer.h"

#include <liburing.h>

#include "core/blocking_pool.hpp"
#include "core/core.hpp"
#include "core/task_group.hpp"
#include "core/io_helpers.hpp"
#include "io.hpp"
#include "logger.hpp"
#include "net.hpp"
#include "tls/handshake.hpp"
#include "tls/socket.hpp"
#include "tls/tls_context.hpp"
