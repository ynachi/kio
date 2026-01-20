//
// Created by Yao ACHI on 19/01/2026.
//

#include "../io.hpp"
#include "../net.hpp"
#include "../kio_logger.hpp"
#include "../executor.hpp"
#include <filesystem>
#include <format>

using namespace uring;
namespace fs = std::filesystem;

static Task<> serve_file(ThreadContext& ctx, int client_fd, const fs::path& file_path) {
    // Open file
    const auto fd_res = co_await io::openat(ctx, file_path, O_RDONLY, 0);

    if (!fd_res) {
        std::string err_response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        co_await io::write_exact(ctx, client_fd,
            std::span{err_response.data(), err_response.size()});
        co_return;
    }

    int file_fd = *fd_res;

    // Get file size
    struct stat st{};
    fstat(file_fd, &st);
    off_t file_size = st.st_size;

    // Send HTTP header
    std::string header = std::format(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: {}\r\n"
        "\r\n",
        file_size
    );

    co_await io::write_exact(ctx, client_fd,
        std::span{header.data(), header.size()});

    // Zero-copy transfer using sendfile
    Log::info("Transferring {} bytes via sendfile", file_size);

    auto sendfile_res = co_await io::sendfile(ctx, client_fd, file_fd,
        0, file_size);

    if (!sendfile_res) {
        Log::error("Sendfile failed: {}", sendfile_res.error().message());
    } else {
        Log::info("File transfer complete");
    }

    co_await io::close(ctx, file_fd);
}

Task<> handle_file_request(ThreadContext& ctx, int client_fd) {
    char buf[4096];

    auto n_res = co_await io::read(ctx, client_fd, buf);
    if (!n_res || *n_res == 0) {
        co_await io::close(ctx, client_fd);
        co_return;
    }

    std::string_view request(buf, *n_res);

    // Parse path from "GET /file.txt HTTP/1.1"
    size_t path_start = request.find('/');
    size_t path_end = request.find(' ', path_start);

    if (path_start != std::string_view::npos && path_end != std::string_view::npos) {
        std::string path = std::string(request.substr(path_start + 1, path_end - path_start - 1));

        // Sanitize path (basic security)
        if (path.find("..") == std::string::npos) {
            fs::path file_path = fs::path("./files") / path;
            co_await serve_file(ctx, client_fd, file_path);
        }
    }

    co_await io::close(ctx, client_fd);
}

int main() {
    // Create files directory
    fs::create_directories("./files");

    // Setup Runtime
    RuntimeConfig config;
    config.num_threads = 1;
    Runtime rt(config);
    rt.loop_forever();

    auto listener = net::TcpListener::bind(8080);
    if (!listener) return 1;

    Log::info("File server listening on http://localhost:8080\n");
    Log::info("Serving files from ./files/\n");

    ThreadContext& ctx = rt.next_thread();;

    auto accept_loop = [&]() -> Task<> {
        while (true) {
            auto client = co_await io::accept(ctx, listener->get());
            if (!client) break;

            ctx.schedule(handle_file_request(ctx, *client));
        }
    };

    block_on(ctx, accept_loop());
    return 0;
}
