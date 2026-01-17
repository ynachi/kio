////////////////////////////////////////////////////////////////////////////////
// example.cpp - Usage examples for uring_exec
//
// Build: g++ -std=c++23 -o example example.cpp -luring
////////////////////////////////////////////////////////////////////////////////

#include "uring_exec.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <vector>

using namespace uring;

////////////////////////////////////////////////////////////////////////////////
// Example 1: Simple Storage Engine Pattern
////////////////////////////////////////////////////////////////////////////////

// Simulates writing a WAL entry then syncing
Task<> write_wal_entry(Executor& ex, int fd, uint64_t offset,
                       const void* data, size_t len) {
    auto res = co_await write(ex, fd, data, len, offset);
    if (!res) {
        std::fprintf(stderr, "write failed: %s\n", std::strerror(res.error()));
        co_return;
    }
    std::printf("Wrote %d bytes at offset %lu\n", *res, offset);
    
    // Datasync is usually sufficient for WAL (metadata doesn't matter)
    auto sync_res = co_await fsync(ex, fd, /*datasync=*/true);
    if (!sync_res) {
        std::fprintf(stderr, "fsync failed: %s\n", std::strerror(sync_res.error()));
        co_return;
    }
    std::printf("Synced successfully\n");
}

// Batched read pattern (common in storage engines)
Task<> read_blocks(Executor& ex, int fd,
                   std::vector<std::pair<uint64_t, std::vector<char>>>& blocks) {
    for (auto& [offset, buffer] : blocks) {
        auto res = co_await read(ex, fd, buffer.data(), buffer.size(), offset);
        if (!res) {
            std::fprintf(stderr, "read at %lu failed: %s\n", offset, std::strerror(res.error()));
            continue;
        }
        std::printf("Read %d bytes from offset %lu\n", *res, offset);
    }
}

void storage_example() {
    std::printf("\n=== Storage Example ===\n");
    
    // Create a test file
    int fd = open("/tmp/uring_test.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::perror("open");
        return;
    }
    
    Executor ex;
    
    // Write some data
    const char* data = "Hello, io_uring storage!";
    ex.spawn(write_wal_entry(ex, fd, 0, data, std::strlen(data)));
    ex.run();
    
    // Read it back with a block pattern
    std::vector<std::pair<uint64_t, std::vector<char>>> blocks = {
        {0, std::vector<char>(8)},
        {8, std::vector<char>(8)},
    };
    ex.spawn(read_blocks(ex, fd, blocks));
    ex.run();
    
    ::close(fd);
    unlink("/tmp/uring_test.dat");
}

////////////////////////////////////////////////////////////////////////////////
// Example 2: Echo Proxy Pattern
////////////////////////////////////////////////////////////////////////////////

Task<> handle_connection(Executor& ex, int client_fd) {
    char buf[4096];
    
    while (true) {
        auto recv_res = co_await recv(ex, client_fd, buf, sizeof(buf));
        if (!recv_res || *recv_res == 0) {
            // Error or connection closed
            break;
        }
        
        int n = *recv_res;
        std::printf("Received %d bytes\n", n);
        
        // Echo back
        auto send_res = co_await send(ex, client_fd, buf, n);
        if (!send_res) {
            std::fprintf(stderr, "send failed: %s\n", std::strerror(send_res.error()));
            break;
        }
    }
    
    co_await close(ex, client_fd);
    std::printf("Connection closed\n");
}

Task<> accept_loop(Executor& ex, int listen_fd, int max_connections) {
    for (int i = 0; i < max_connections; ++i) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        auto res = co_await accept(ex, listen_fd, 
                                   reinterpret_cast<sockaddr*>(&client_addr), 
                                   &addr_len);
        if (!res) {
            std::fprintf(stderr, "accept failed: %s\n", std::strerror(res.error()));
            continue;
        }
        
        int client_fd = *res;
        std::printf("Accepted connection from %s:%d (fd=%d)\n",
                    inet_ntoa(client_addr.sin_addr),
                    ntohs(client_addr.sin_port),
                    client_fd);
        
        // Spawn handler - runs concurrently
        ex.spawn(handle_connection(ex, client_fd));
    }
}

void proxy_example() {
    std::printf("\n=== Proxy Example (accepts 2 connections then exits) ===\n");
    std::printf("Connect with: nc localhost 8080\n\n");
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        return;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(listen_fd);
        return;
    }
    
    if (listen(listen_fd, 128) < 0) {
        std::perror("listen");
        ::close(listen_fd);
        return;
    }
    
    std::printf("Listening on port 8080...\n");
    
    Executor ex;
    ex.spawn(accept_loop(ex, listen_fd, 2));  // Accept 2 connections for demo
    ex.run();
    
    ::close(listen_fd);
}

////////////////////////////////////////////////////////////////////////////////
// Example 3: Timeout Pattern
////////////////////////////////////////////////////////////////////////////////

Task<> timeout_example_task(Executor& ex) {
    std::printf("Waiting 500ms...\n");
    co_await timeout_ms(ex, 500);
    std::printf("Done waiting!\n");
}

void timeout_example() {
    std::printf("\n=== Timeout Example ===\n");
    
    Executor ex;
    ex.spawn(timeout_example_task(ex));
    ex.run();
}

////////////////////////////////////////////////////////////////////////////////
// Example 4: Concurrent Operations
////////////////////////////////////////////////////////////////////////////////

Task<int> delayed_value(Executor& ex, int value, uint64_t delay_ms) {
    co_await timeout_ms(ex, delay_ms);
    std::printf("Returning value %d after %lums\n", value, delay_ms);
    co_return value;
}

Task<> concurrent_example_task(Executor& ex) {
    // These run concurrently - we just can't await them simultaneously 
    // without additional machinery (like WhenAll)
    // For now, sequential but non-blocking:
    
    std::printf("Starting concurrent tasks...\n");
    
    int a = co_await delayed_value(ex, 1, 300);
    int b = co_await delayed_value(ex, 2, 200);
    int c = co_await delayed_value(ex, 3, 100);
    
    std::printf("Results: %d, %d, %d (sum=%d)\n", a, b, c, a + b + c);
}

void concurrent_example() {
    std::printf("\n=== Concurrent Example ===\n");
    
    Executor ex;
    ex.spawn(concurrent_example_task(ex));
    ex.run();
}

////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
    std::printf("uring_exec examples\n");
    std::printf("===================\n");
    if (argc > 1 && std::strcmp(argv[1], "proxy") == 0) {
        proxy_example();
    } else {
        storage_example();
        timeout_example();
        concurrent_example();
    }
    
    return 0;
}
