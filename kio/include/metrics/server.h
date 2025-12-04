//
// Created by Yao ACHI on 02/12/2025.
//

#ifndef KIO_SERVER_H
#define KIO_SERVER_H
#include <string_view>

#include "kio/include/io/worker.h"
#include "kio/include/net.h"

namespace kio
{
    static constexpr size_t kMetricServerWorkerId = 60965;
    static constexpr size_t kMaxRequestSize = 8192;

    namespace detail
    {
        enum class HttpStatusCode : int
        {
            OK = 200,
            BadRequest = 400,
            NotFound = 404,
            MethodNotAllowed = 405,
            InternalServerError = 500
        };

        constexpr std::string_view to_string(const HttpStatusCode code)
        {
            switch (code)
            {
                case HttpStatusCode::OK:
                    return "OK";
                case HttpStatusCode::BadRequest:
                    return "Bad Request";
                case HttpStatusCode::NotFound:
                    return "Not Found";
                case HttpStatusCode::MethodNotAllowed:
                    return "Method Not Allowed";
                case HttpStatusCode::InternalServerError:
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
         * @param config The worker configuration. The worker ID will be set to 60,965 to distinguish it from actual user workers.
         */
        explicit MetricsServer(std::string bind_addr = "127.0.0.1", uint16_t port = 9090, io::WorkerConfig config = {});

        ~MetricsServer() { stop(); }

        // Non-copyable
        MetricsServer(const MetricsServer&) = delete;
        MetricsServer& operator=(const MetricsServer&) = delete;

        /**
         * @brief Start the metrics server
         */
        void start();

        /**
         * @brief Stop the metrics server
         */
        void stop();

        /**
         * @brief Get the server address
         */
        [[nodiscard]] std::string address() const { return std::format("http://{}:{}/metrics", bind_addr_, port_); }

    private:
        static DetachedTask accept_loop(io::Worker& worker, int server_fd);
        std::string bind_addr_;
        uint16_t port_;
        std::jthread ev_thread_;

        std::unique_ptr<io::Worker> worker_;
        int server_fd_{-1};

        // HTTP request handler
        static DetachedTask handle_client(io::Worker& worker, net::FDGuard fd);

        // HTTP response builders
        static std::string build_response(detail::HttpStatusCode code, std::string_view content_type, std::string_view body);
    };
}  // namespace kio

#endif  // KIO_SERVER_H
