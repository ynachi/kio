//
// Created by Yao ACHI on 25/10/2025.
//

#include <gtest/gtest.h>
#include <benchmark/benchmark.h>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "core/include/io/worker.h"
#include "core/include/net.h"
#include "core/include/sync_wait.h"

using namespace kio;
using namespace kio::io;
using namespace kio::net;

// --- Server Task ---
// A simple echo server coroutine
DetachedTask echo_server(Worker &worker, int listen_fd) {
    auto client_fd_exp = co_await worker.async_accept(listen_fd, nullptr, nullptr);
    if (!client_fd_exp) {
        spdlog::error("Benchmark server accept failed");
        co_return;
    }
    int client_fd = *client_fd_exp;

    char buffer[1024];
    const auto st = worker.get_stop_token();

    while (!st.stop_requested()) {
        auto read_res = co_await worker.async_read(client_fd, std::span(buffer), 0);
        if (!read_res || *read_res == 0) {
            break; // Client disconnected or error
        }

        auto write_res = co_await worker.async_write(client_fd, std::span(buffer, *read_res), 0);
        if (!write_res) {
            break; // Error
        }
    }
    ::close(client_fd);
}

// --- Benchmark Fixture ---
class NetworkEchoFixture : public benchmark::Fixture {
public:
    int server_fd = -1;
    int client_fd = -1;
    uint16_t port = 0;
    std::unique_ptr<Worker> server_worker;
    std::unique_ptr<std::jthread> server_thread;

    void SetUp(const ::benchmark::State &state) override {
        spdlog::set_level(spdlog::level::off);

        // 1. Create a server socket on a random port
        auto server_fd_exp = create_tcp_socket("127.0.0.1", 0, 1);
        EXPECT_TRUE(server_fd_exp.has_value());
        server_fd = *server_fd_exp;

        // Get the randomly assigned port
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        ASSERT_EQ(getsockname(server_fd, (struct sockaddr *) &addr, &len), 0);
        port = ntohs(addr.sin_port);

        // 2. Start the server worker
        WorkerConfig config;
        server_worker = std::make_unique<Worker>(0, config, [this](Worker &w) {
            echo_server(w, server_fd).detach();
        });
        server_thread = std::make_unique<std::jthread>([this] {
            server_worker->loop_forever();
        });
        server_worker->wait_ready();

        // 3. Create a blocking client socket
        client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(client_fd, 0);
        addr.sin_port = htons(port);
        ASSERT_EQ(::connect(client_fd, (struct sockaddr *) &addr, sizeof(addr)), 0);
    }

    void TearDown(const ::benchmark::State &state) override {
        (void) server_worker->request_stop();
        server_thread.reset();
        server_worker.reset();
        ::close(client_fd);
        ::close(server_fd);
    }
};

BENCHMARK_F(NetworkEchoFixture, BM_Echo_Throughput)(benchmark::State &state) {
    // The data we will send and expect back
    std::string payload = "ping";
    std::span<const char> write_buf(payload.data(), payload.size());
    char read_buf[16];
    std::span<char> read_span(read_buf);

    for (auto _: state) {
        // This is the part being measured: a full round trip
        ssize_t written = ::write(client_fd, write_buf.data(), write_buf.size());
        if (written != 4) state.SkipWithError("Client write failed");

        ssize_t bytes_read = 0;
        while (bytes_read < 4) {
            ssize_t n = ::read(client_fd, read_span.data() + bytes_read, read_span.size() - bytes_read);
            if (n <= 0) state.SkipWithError("Client read failed");
            bytes_read += n;
        }

        benchmark::ClobberMemory(); // Prevent compiler from optimizing away
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * (payload.size() * 2)); // In + Out
}
