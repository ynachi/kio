//
// Created by Yao ACHI on 02/12/2025.
//

#include "kio/metrics/server.h"

#include <unistd.h>
#include <utility>

#include "../net/net.h"
#include "kio/core/async_logger.h"
#include "kio/core/bytes_mut.h"
#include "kio/core/worker.h"
#include "kio/metrics/registry.h"
#include "kio/sync/sync_wait.h"

namespace kio
{
    using namespace kio::io;
    using namespace kio::net;
    using namespace kio::detail;

    namespace
    {
        struct HttpRequest
        {
            std::string_view method;
            std::string_view path;
            bool complete{false};
            bool valid{false};
            size_t header_length{0};

            // Returns a parsed request object.
            // .complete = true if we found \r\n\r\n
            static HttpRequest parse(std::span<const char> data)
            {
                HttpRequest req;
                std::string_view view(data.data(), data.size());

                // Check for the end of headers
                const auto header_end = view.find("\r\n\r\n");
                if (header_end == std::string_view::npos)
                {
                    return req;  // Incomplete
                }

                req.complete = true;
                req.header_length = header_end + 4;  // Skip the \r\n\r\n

                // 2. Parse Request Line
                const auto line_end = view.find("\r\n");
                std::string_view line = view.substr(0, line_end);

                // Method
                const auto first_space = line.find(' ');
                if (first_space == std::string_view::npos) return req;
                req.method = line.substr(0, first_space);

                // Path
                const auto second_space = line.find(' ', first_space + 1);
                if (second_space == std::string_view::npos) return req;

                req.path = line.substr(first_space + 1, second_space - first_space - 1);
                req.valid = true;
                return req;
            }
        };

    }  // anonymous namespace

    MetricsServer::MetricsServer(std::string bind_addr, const uint16_t port, io::WorkerConfig config) : bind_addr_(std::move(bind_addr)), port_(port)
    {
        auto res = create_tcp_socket(bind_addr_, port_, 128);
        if (!res.has_value())
        {
            ALOG_ERROR("Failed to create metrics server socket: {}", res.error());
            std::terminate();
        }

        server_fd_ = res.value();
        ALOG_INFO("Metrics server listening on endpoint: {}:{}, FD:{}", bind_addr_, port_, server_fd_);

        worker_ = std::make_unique<io::Worker>(kMetricServerWorkerId, config, [this](io::Worker& w) { accept_loop(w, server_fd_).detach(); });
    }

    void MetricsServer::start()
    {
        if (ev_thread_.joinable())
        {
            ALOG_WARN("MetricsServer already running");
            return;
        }

        // start the event loop in a thread
        ev_thread_ = std::jthread([&] { worker_->loop_forever(); });
    }

    void MetricsServer::stop()
    {
        if (worker_ == nullptr)
        {
            ALOG_WARN("MetricsServer is not running");
            return;
        }

        if (!worker_->is_running())
        {
            ALOG_WARN("MetricsServer is not running");
            return;
        }

        ALOG_INFO("Stopping MetricsServer...");

        // Stop worker
        if (worker_->request_stop())
        {
            worker_->wait_shutdown();
        }
        else
        {
            ALOG_WARN("Failed to stop worker");
            // manually join the worker thread
            ev_thread_.join();
        }

        // Close server socket
        if (server_fd_ >= 0)
        {
            ::close(server_fd_);
            server_fd_ = -1;
        }

        ALOG_INFO("MetricsServer stopped");
    }

    DetachedTask MetricsServer::accept_loop(Worker& worker, const int server_fd)
    {
        const auto st = worker.get_stop_token();

        while (!st.stop_requested())
        {
            sockaddr_storage client_addr{};
            socklen_t addr_len = sizeof(client_addr);

            auto res = co_await worker.async_accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

            if (!res.has_value())
            {
                if (!st.stop_requested())
                {
                    ALOG_WARN("Metrics accept failed: {}", res.error().value);
                }
                continue;
            }

            FDGuard client_fd(res.value());
            handle_client(worker, std::move(client_fd)).detach();
        }
    }

    DetachedTask MetricsServer::handle_client(Worker& worker, FDGuard fd)
    {
        // Use BytesMut for dynamic buffering
        BytesMut buffer(1024);

        while (true)
        {
            // Ensure we have space
            buffer.reserve(1024);
            auto dst = buffer.writable_span();

            // Read
            const auto read_res = co_await worker.async_read(fd.get(), dst);

            if (!read_res.has_value() || read_res.value() == 0)
            {
                co_return;  // Closed or Error
            }

            // Update buffer cursor
            buffer.commit_write(read_res.value());

            // Try Parse
            auto req = HttpRequest::parse(buffer.readable_span());

            if (req.complete)
            {
                // We have a full HTTP request
                std::string response;

                if (!req.valid)
                {
                    response = build_response(HttpStatusCode::BadRequest, "text/plain", "Bad Request");
                }
                else if (req.method != "GET")
                {
                    response = build_response(HttpStatusCode::MethodNotAllowed, "text/plain", "Method Not Allowed");
                }
                else if (req.path == "/metrics")
                {
                    try
                    {
                        std::string body = MetricsRegistry<>::Instance().Scrape();
                        response = build_response(HttpStatusCode::OK, "text/plain; version=0.0.4", body);
                    }
                    catch (const std::exception& e)
                    {
                        response = build_response(HttpStatusCode::InternalServerError, "text/plain", e.what());
                    }
                }
                else if (req.path == "/health" || req.path == "/")
                {
                    response = build_response(HttpStatusCode::OK, "text/plain", "OK");
                }
                else
                {
                    response = build_response(HttpStatusCode::NotFound, "text/plain", "Not Found");
                }

                // Send response
                co_await worker.async_write_exact(fd.get(), std::span(response.data(), response.size()));

                // For a simple metrics server, we close after one request (Connection: close)
                // Another option would have been keepalive
                break;
            }

            // If the buffer gets too big without finding a delimiter, kill the connection
            if (buffer.remaining() > kMaxRequestSize)
            {
                ALOG_WARN("Metrics request too large, closing connection");
                break;
            }
        }
        // fd destructor closes socket
    }

    std::string MetricsServer::build_response(HttpStatusCode code, std::string_view content_type, std::string_view body)
    {
        return std::format(
                "HTTP/1.1 {} {}\r\n"
                "Content-Type: {}\r\n"
                "Content-Length: {}\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{}",
                static_cast<int>(code), to_string(code), content_type, body.size(), body);
    }

}  // namespace kio
