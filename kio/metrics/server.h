//
// Created by Yao ACHI on 02/12/2025.
//

#ifndef KIO_SERVER_H
#define KIO_SERVER_H
#include <string_view>
#include <sys/types.h>

#include "kio/core/worker.h"
#include "kio/net/net.h"

namespace kio
{
static constexpr size_t kMetricServerWorkerId = 60965;
static constexpr size_t kMaxRequestSize = 8192;

namespace detail
{
enum class HttpStatusCode : uint16_t
{
    kOk = 200,
    kBadRequest = 400,
    kNotFound = 404,
    kMethodNotAllowed = 405,
    kInternalServerError = 500
};

constexpr std::string_view ToString(const HttpStatusCode code)
{
    switch (code)
    {
        case HttpStatusCode::kOk:
            return "OK";
        case HttpStatusCode::kBadRequest:
            return "Bad Request";
        case HttpStatusCode::kNotFound:
            return "Not Found";
        case HttpStatusCode::kMethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatusCode::kInternalServerError:
            return "Internal Server Error";
        default:
            return "Unknown";
    }
}
}  // namespace detail

/**
 * @brief A lightweight, embedded HTTP server for exposing Prometheus metrics.
 *
 * Example usage:
 * @code
 * MetricsServer server("0.0.0.0", 9090);
 * server.start();
 * // ... application runs ...
 * server.stop();
 * @endcode
 */
class MetricsServer
{
public:
    /**
     * @brief Construct a metrics server
     * @param bind_addr IP address to bind (e.g., "0.0.0.0", "127.0.0.1")
     * @param port Port to listen on (default: 9090)
     * @param config The worker configuration. The worker ID will be set to 60,965 to distinguish it from actual user
     * workers.
     */
    explicit MetricsServer(std::string bind_addr = "127.0.0.1", uint16_t port = 9090, io::WorkerConfig config = {});

    ~MetricsServer() { Stop(); }

    // Non-copyable
    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    /**
     * @brief Start the metrics server
     */
    void Start();

    /**
     * @brief Stop the metrics server
     */
    void Stop();

    /**
     * @brief Get the server address
     */
    [[nodiscard]] std::string Address() const { return std::format("http://{}:{}/metrics", bind_addr_, port_); }

private:
    static DetachedTask AcceptLoop(io::Worker& worker, int server_fd);
    std::string bind_addr_;
    uint16_t port_;
    std::unique_ptr<io::Worker> worker_;
    std::jthread ev_thread_;
    int server_fd_{-1};

    // HTTP request handler
    static DetachedTask HandleClient(io::Worker& worker, net::FDGuard fd);

    // HTTP response builders
    static std::string BuildResponse(detail::HttpStatusCode code, std::string_view content_type, std::string_view body);
};
}  // namespace kio

#endif  // KIO_SERVER_H
