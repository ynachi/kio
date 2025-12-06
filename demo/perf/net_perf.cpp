//
// KIO Network Benchmark - Equivalent to Photon's performance test
//
// Usage:
//   Server mode: ./kio_benchmark --port 9527 --vcpu_num 4
//   Client pingpong: ./kio_benchmark --client --client_mode pingpong --ip 127.0.0.1 --port 9527 --vcpu_num 4 --client_connection_num 100
//   Client streaming: ./kio_benchmark --client --client_mode streaming --ip 127.0.0.1 --port 9527
//

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <gflags/gflags.h>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <vector>

#include "../../kio/core/async_logger.h"
#include "../../kio/core/coro.h"
#include "../../kio/core/worker.h"
#include "../../kio/core/worker_pool.h"
#include "../../kio/net/net.h"
#include "../../kio/sync/sync_wait.h"

using namespace kio;
using namespace kio::io;

// Command line flags - using gflags like Photon
DEFINE_uint64(show_statistics_interval, 1, "interval seconds to show statistics");
DEFINE_bool(client, false, "client or server? default is server");
DEFINE_string(client_mode, "pingpong", "client mode. Choose between streaming or pingpong");
DEFINE_uint64(client_connection_num, 100, "number of the connections of the client (shared by all vCPUs), only available in pingpong mode");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 512, "buffer size");
DEFINE_uint64(vcpu_num, 1, "vCPU number for both client and server");
DEFINE_uint64(uring_queue_depth, 16384, "io_uring queue depth");
DEFINE_uint64(default_op_slots, 8192, "default operation slots for coroutine tracking");

// Global statistics
static std::atomic<bool> g_stop_test{false};
static std::atomic<uint64_t> g_qps{0};
static std::atomic<uint64_t> g_time_cost_us{0};

// Signal handler for graceful shutdown
void handle_signal(int sig)
{
    ALOG_INFO("Signal {} received. Stopping test gracefully...", sig);
    g_stop_test.store(true, std::memory_order_release);
}

// Statistics reporting loop
DetachedTask run_statistics_loop(bool show_latency)
{
    while (!g_stop_test.load(std::memory_order_acquire))
    {
        co_await std::suspend_always{};  // Yield to avoid tight loop
        std::this_thread::sleep_for(std::chrono::seconds(FLAGS_show_statistics_interval));

        uint64_t qps_val = g_qps.exchange(0, std::memory_order_acq_rel);
        uint64_t time_val = g_time_cost_us.exchange(0, std::memory_order_acq_rel);

        if (show_latency && qps_val > 0)
        {
            uint64_t avg_latency_us = time_val / qps_val;
            double qps_per_sec = static_cast<double>(qps_val) / FLAGS_show_statistics_interval;
            double bw_mbps = (qps_per_sec * FLAGS_buf_size) / (1024.0 * 1024.0);

            ALOG_INFO("qps: {:.2f}, bw: {:.2f} MB/s, latency: {} us", qps_per_sec, bw_mbps, avg_latency_us);
        }
        else
        {
            double qps_per_sec = static_cast<double>(qps_val) / FLAGS_show_statistics_interval;
            double bw_mbps = (qps_per_sec * FLAGS_buf_size) / (1024.0 * 1024.0);

            ALOG_INFO("qps: {:.2f}, bw: {:.2f} MB/s", qps_per_sec, bw_mbps);
        }
    }
}

//=============================================================================
// CLIENT IMPLEMENTATIONS
//=============================================================================

// Ping-pong client: send data, wait for echo, repeat
Task<void> ping_pong_worker(Worker& worker, size_t conn_index)
{
    co_await SwitchToWorker(worker);

    // Connect to server
    net::SocketAddress server_addr_result = co_await []() -> Task<net::SocketAddress>
    {
        auto addr = net::parse_address(FLAGS_ip, FLAGS_port);
        if (!addr)
        {
            throw std::runtime_error(std::format("Failed to parse server address: {}", addr.error()));
        }
        co_return *addr;
    }();

    auto sock_result = net::create_raw_socket(server_addr_result.family);
    if (!sock_result)
    {
        ALOG_ERROR("Failed to create socket: {}", sock_result.error());
        co_return;
    }

    int client_fd = *sock_result;
    net::FDGuard fd_guard(client_fd);

    // Connect
    auto connect_res = co_await worker.async_connect(client_fd, reinterpret_cast<const sockaddr*>(&server_addr_result.addr), server_addr_result.addrlen);

    if (!connect_res)
    {
        ALOG_ERROR("Connection {} failed: {}", conn_index, connect_res.error());
        co_return;
    }

    ALOG_DEBUG("Connection {} established on fd {}", conn_index, client_fd);

    // Allocate buffer
    std::vector<char> buffer(FLAGS_buf_size);
    std::memset(buffer.data(), 'A', buffer.size());

    auto stop_token = worker.get_stop_token();

    // Ping-pong loop
    while (!g_stop_test.load(std::memory_order_acquire) && !stop_token.stop_requested())
    {
        auto start = std::chrono::steady_clock::now();

        // Send
        auto write_res = co_await worker.async_write_exact(client_fd, std::span(buffer));
        if (!write_res)
        {
            ALOG_DEBUG("Write failed on connection {}: {}", conn_index, write_res.error());
            break;
        }

        // Receive echo
        auto read_res = co_await worker.async_read_exact(client_fd, std::span(buffer));
        if (!read_res)
        {
            ALOG_DEBUG("Read failed on connection {}: {}", conn_index, read_res.error());
            break;
        }

        auto end = std::chrono::steady_clock::now();
        auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        g_time_cost_us.fetch_add(latency_us, std::memory_order_relaxed);
        g_qps.fetch_add(1, std::memory_order_relaxed);
    }

    ALOG_DEBUG("Connection {} terminated", conn_index);
}

// Streaming client: continuous send and receive in parallel
Task<void> streaming_client_worker(Worker& worker)
{
    co_await SwitchToWorker(worker);

    // Connect to server
    net::SocketAddress server_addr_result = co_await []() -> Task<net::SocketAddress>
    {
        auto addr = net::parse_address(FLAGS_ip, FLAGS_port);
        if (!addr)
        {
            throw std::runtime_error(std::format("Failed to parse server address: {}", addr.error()));
        }
        co_return *addr;
    }();

    auto sock_result = net::create_raw_socket(server_addr_result.family);
    if (!sock_result)
    {
        ALOG_ERROR("Failed to create socket: {}", sock_result.error());
        co_return;
    }

    int client_fd = *sock_result;
    net::FDGuard fd_guard(client_fd);

    auto connect_res = co_await worker.async_connect(client_fd, reinterpret_cast<const sockaddr*>(&server_addr_result.addr), server_addr_result.addrlen);

    if (!connect_res)
    {
        ALOG_ERROR("Connection failed: {}", connect_res.error());
        co_return;
    }

    ALOG_INFO("Streaming connection established on fd {}", client_fd);

    // Spawn sender and receiver coroutines
    auto sender = [](Worker& w, int fd) -> DetachedTask
    {
        co_await SwitchToWorker(w);
        std::vector<char> buffer(FLAGS_buf_size);
        std::memset(buffer.data(), 'S', buffer.size());

        auto stop_token = w.get_stop_token();
        while (!g_stop_test.load(std::memory_order_acquire) && !stop_token.stop_requested())
        {
            auto res = co_await w.async_write_exact(fd, std::span(buffer));
            if (!res)
            {
                ALOG_DEBUG("Sender write failed: {}", res.error());
                break;
            }
        }
        ALOG_DEBUG("Sender terminated");
    };

    auto receiver = [](Worker& w, int fd) -> DetachedTask
    {
        co_await SwitchToWorker(w);
        std::vector<char> buffer(FLAGS_buf_size);

        auto stop_token = w.get_stop_token();
        while (!g_stop_test.load(std::memory_order_acquire) && !stop_token.stop_requested())
        {
            auto res = co_await w.async_read_exact(fd, std::span(buffer));
            if (!res)
            {
                ALOG_DEBUG("Receiver read failed: {}", res.error());
                break;
            }
            g_qps.fetch_add(1, std::memory_order_relaxed);
        }
        ALOG_DEBUG("Receiver terminated");
    };

    sender(worker, client_fd).detach();
    receiver(worker, client_fd).detach();

    // Keep connection alive
    auto stop_token = worker.get_stop_token();
    while (!g_stop_test.load(std::memory_order_acquire) && !stop_token.stop_requested())
    {
        co_await worker.async_sleep(std::chrono::milliseconds(100));
    }
}

int run_ping_pong_client()
{
    ALOG_INFO("Starting ping-pong client with {} connections across {} workers", FLAGS_client_connection_num, FLAGS_vcpu_num);

    WorkerConfig worker_config{};
    worker_config.uring_queue_depth = FLAGS_uring_queue_depth;
    worker_config.default_op_slots = FLAGS_default_op_slots;

    IOPool pool(FLAGS_vcpu_num, worker_config);

    // Spawn statistics reporter
    run_statistics_loop(true).detach();

    // Distribute connections across workers
    for (size_t i = 0; i < FLAGS_client_connection_num; ++i)
    {
        size_t worker_idx = i % FLAGS_vcpu_num;
        Worker* worker = pool.get_worker(worker_idx);
        if (worker)
        {
            worker->post([worker, i]() -> std::coroutine_handle<> { return ping_pong_worker(*worker, i).h_; }());
        }
    }

    ALOG_INFO("Ping-pong client running. Press Ctrl+C to stop.");

    // Wait for stop signal
    while (!g_stop_test.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pool.stop();
    ALOG_INFO("Ping-pong client stopped");
    return 0;
}

int run_streaming_client()
{
    ALOG_INFO("Starting streaming client on worker 0");

    WorkerConfig worker_config{};
    worker_config.uring_queue_depth = FLAGS_uring_queue_depth;
    worker_config.default_op_slots = FLAGS_default_op_slots;

    IOPool pool(1, worker_config);

    // Spawn statistics reporter
    run_statistics_loop(false).detach();

    Worker* worker = pool.get_worker(0);
    if (worker)
    {
        worker->post([worker]() -> std::coroutine_handle<> { return streaming_client_worker(*worker).h_; }());
    }

    ALOG_INFO("Streaming client running. Press Ctrl+C to stop.");

    // Wait for stop signal
    while (!g_stop_test.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pool.stop();
    ALOG_INFO("Streaming client stopped");
    return 0;
}

//=============================================================================
// SERVER IMPLEMENTATION
//=============================================================================

// Echo handler for each client connection
DetachedTask handle_client_connection(Worker& worker, int client_fd)
{
    co_await SwitchToWorker(worker);

    std::vector<char> buffer(FLAGS_buf_size);
    auto stop_token = worker.get_stop_token();

    while (!g_stop_test.load(std::memory_order_acquire) && !stop_token.stop_requested())
    {
        // Read from client
        auto read_res = co_await worker.async_read(client_fd, std::span(buffer));
        if (!read_res)
        {
            ALOG_DEBUG("Read error: {}", read_res.error());
            break;
        }

        ssize_t n = *read_res;
        if (n == 0)
        {
            ALOG_DEBUG("Client disconnected");
            break;
        }

        // Echo back
        auto write_res = co_await worker.async_write_exact(client_fd, std::span(buffer.data(), n));
        if (!write_res)
        {
            ALOG_DEBUG("Write error: {}", write_res.error());
            break;
        }

        g_qps.fetch_add(1, std::memory_order_relaxed);
    }

    close(client_fd);
    ALOG_DEBUG("Client handler finished for fd {}", client_fd);
}

// Accept loop for each worker
DetachedTask accept_loop(Worker& worker, int listen_fd)
{
    co_await SwitchToWorker(worker);

    ALOG_INFO("Worker {} started accepting connections on port {}", worker.get_id(), FLAGS_port + worker.get_id());

    auto stop_token = worker.get_stop_token();

    while (!g_stop_test.load(std::memory_order_acquire) && !stop_token.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        auto accept_res = co_await worker.async_accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!accept_res)
        {
            if (g_stop_test.load(std::memory_order_acquire))
            {
                break;
            }
            ALOG_ERROR("Accept error: {}", accept_res.error());
            continue;
        }

        int client_fd = *accept_res;
        ALOG_DEBUG("Worker {} accepted connection on fd {}", worker.get_id(), client_fd);

        // Spawn handler for this connection
        handle_client_connection(worker, client_fd).detach();
    }

    ALOG_INFO("Worker {} stopped accepting connections", worker.get_id());
}

int run_echo_server()
{
    ALOG_INFO("Starting echo server with {} workers on port {}", FLAGS_vcpu_num, FLAGS_port);

    // Setup signal handlers
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    WorkerConfig worker_config{};
    worker_config.uring_queue_depth = FLAGS_uring_queue_depth;
    worker_config.default_op_slots = FLAGS_default_op_slots;

    // Create listening sockets (one per worker for SO_REUSEPORT)
    std::vector<int> listen_fds;
    for (size_t i = 0; i < FLAGS_vcpu_num; ++i)
    {
        auto fd_result = net::create_tcp_socket("0.0.0.0", FLAGS_port + i, 4096);
        if (!fd_result)
        {
            ALOG_ERROR("Failed to create listening socket {}: {}", i, fd_result.error());
            return 1;
        }
        listen_fds.push_back(*fd_result);
        ALOG_INFO("Created listening socket {} on port {}", i, FLAGS_port + i);
    }

    // Create worker pool with accept loops
    IOPool pool(FLAGS_vcpu_num, worker_config,
                [&listen_fds](Worker& worker)
                {
                    size_t worker_id = worker.get_id();
                    if (worker_id < listen_fds.size())
                    {
                        accept_loop(worker, listen_fds[worker_id]).detach();
                    }
                });

    // Spawn statistics reporter
    run_statistics_loop(false).detach();

    ALOG_INFO("Server running. Press Ctrl+C to stop.");

    // Wait for stop signal
    while (!g_stop_test.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ALOG_INFO("Stopping server...");
    pool.stop();

    // Close listening sockets
    for (int fd: listen_fds)
    {
        close(fd);
    }

    ALOG_INFO("Server stopped");
    return 0;
}

//=============================================================================
// MAIN
//=============================================================================

int main(int argc, char** argv)
{
    // Parse command line flags using gflags (like Photon)
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Setup logging - use Info level like Photon
    alog::configure(4096, LogLevel::Info);

    ALOG_INFO("=== KIO Network Benchmark ===");
    ALOG_INFO("Mode: {}", FLAGS_client ? "Client" : "Server");
    ALOG_INFO("Buffer size: {} bytes", FLAGS_buf_size);
    ALOG_INFO("Workers: {}", FLAGS_vcpu_num);
    ALOG_INFO("io_uring queue depth: {}", FLAGS_uring_queue_depth);

    try
    {
        if (FLAGS_client)
        {
            if (FLAGS_client_mode == "pingpong")
            {
                return run_ping_pong_client();
            }
            else if (FLAGS_client_mode == "streaming")
            {
                return run_streaming_client();
            }
            else
            {
                ALOG_ERROR("Unknown client mode: {}. Choose 'pingpong' or 'streaming'", FLAGS_client_mode);
                return 1;
            }
        }
        else
        {
            return run_echo_server();
        }
    }
    catch (const std::exception& e)
    {
        ALOG_ERROR("Fatal error: {}", e.what());
        return 1;
    }
}
