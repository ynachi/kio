// Throwaway blocking client for bench_net_server echo mode.
// Usage: bench_net_client <port> <conns> <msg-bytes> <seconds>
// One thread per connection; streams fixed-size messages and echoes. Prints
// aggregate throughput.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>
#include <thread>
#include <vector>

namespace
{
    std::atomic<uint64_t> g_bytes{0};
    std::atomic<bool> g_stop{false};

    bool read_full(int fd, char* buf, size_t n)
    {
        size_t got = 0;
        while (got < n)
        {
            const ssize_t r = read(fd, buf + got, n - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }

    bool write_full(int fd, const char* buf, size_t n)
    {
        size_t put = 0;
        while (put < n)
        {
            const ssize_t w = write(fd, buf + put, n - put);
            if (w <= 0) return false;
            put += static_cast<size_t>(w);
        }
        return true;
    }

    void run_conn(int port, unsigned msg)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
        {
            std::perror("connect");
            return;
        }
        std::vector<char> buf(msg, 'x');
        uint64_t bytes = 0;
        while (!g_stop.load(std::memory_order_relaxed))
        {
            if (!write_full(fd, buf.data(), msg) || !read_full(fd, buf.data(), msg)) break;
            bytes += msg;
        }
        g_bytes.fetch_add(bytes, std::memory_order_relaxed);
        close(fd);
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::println(stderr, "usage: {} <port> <conns> <msg-bytes> <seconds>", argv[0]);
        return 1;
    }
    const int port = std::atoi(argv[1]);
    const int conns = std::atoi(argv[2]);
    const unsigned msg = static_cast<unsigned>(std::atoi(argv[3]));
    const int seconds = std::atoi(argv[4]);

    std::vector<std::thread> ts;
    for (int i = 0; i < conns; ++i) ts.emplace_back(run_conn, port, msg);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    g_stop.store(true);
    for (auto& t : ts) t.join();

    const uint64_t bytes = g_bytes.load();
    std::println("echo64k client: {:.1f} MiB/s over {} conns ({} msg)", bytes / 1048576.0, conns,
                 msg);
    return 0;
}
