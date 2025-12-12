//
// Non-KTLS TLS Client - Uses userspace OpenSSL only (no kernel TLS)
// For testing against KTLS-enabled server to isolate the issue
//

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// ANSI colors for output
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define CYAN "\033[36m"
#define RESET "\033[0m"

void log_info(const std::string& msg) { std::cout << CYAN "[INFO] " RESET << msg << std::endl; }
void log_ok(const std::string& msg) { std::cout << GREEN "[OK] " RESET << msg << std::endl; }
void log_error(const std::string& msg) { std::cerr << RED "[ERROR] " RESET << msg << std::endl; }
void log_warn(const std::string& msg) { std::cout << YELLOW "[WARN] " RESET << msg << std::endl; }

std::string get_openssl_error()
{
    std::string result;
    unsigned long err;
    while ((err = ERR_get_error()) != 0)
    {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        if (!result.empty()) result += "; ";
        result += buf;
    }
    return result.empty() ? "Unknown error" : result;
}

int create_tcp_connection(const char* host, int port)
{
    // Resolve hostname
    struct addrinfo hints{}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int ret = getaddrinfo(host, port_str.c_str(), &hints, &result);
    if (ret != 0)
    {
        log_error(std::string("getaddrinfo failed: ") + gai_strerror(ret));
        return -1;
    }

    // Create socket
    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0)
    {
        log_error(std::string("socket() failed: ") + strerror(errno));
        freeaddrinfo(result);
        return -1;
    }

    // Connect
    if (connect(fd, result->ai_addr, result->ai_addrlen) < 0)
    {
        log_error(std::string("connect() failed: ") + strerror(errno));
        close(fd);
        freeaddrinfo(result);
        return -1;
    }

    freeaddrinfo(result);
    return fd;
}

bool check_ktls_status(SSL* ssl)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    BIO* wbio = SSL_get_wbio(ssl);
    BIO* rbio = SSL_get_rbio(ssl);
    bool tx = wbio ? BIO_get_ktls_send(wbio) : false;
    bool rx = rbio ? BIO_get_ktls_recv(rbio) : false;
    return tx || rx;
#else
    return false;
#endif
}

int main(int argc, char* argv[])
{
    // Parse arguments
    const char* host = "127.0.0.1";
    int port = 8080;
    std::string message = "Hello from non-KTLS client!\n";
    bool verbose = false;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)
            port = std::stoi(argv[++i]);
        else if (arg == "--message" && i + 1 < argc)
            message = std::string(argv[++i]) + "\n";
        else if (arg == "-v" || arg == "--verbose")
            verbose = true;
        else if (arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --host HOST     Server host (default: 127.0.0.1)\n"
                      << "  --port PORT     Server port (default: 8080)\n"
                      << "  --message MSG   Message to send (default: 'Hello from non-KTLS client!')\n"
                      << "  -v, --verbose   Verbose output\n";
            return 0;
        }
    }

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           Non-KTLS TLS Client (Userspace Only)               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    log_info(std::string("Target: ") + host + ":" + std::to_string(port));
    log_info(std::string("OpenSSL: ") + OpenSSL_version(OPENSSL_VERSION));

    // Initialize OpenSSL
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    // Create SSL context - DO NOT enable KTLS
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
    {
        log_error("Failed to create SSL_CTX: " + get_openssl_error());
        return 1;
    }

    // Explicitly DO NOT set SSL_OP_ENABLE_KTLS
    // This ensures we use userspace TLS only
    log_info("KTLS explicitly DISABLED - using userspace TLS only");

    // Set minimum TLS version to 1.2
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // Disable certificate verification for testing
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    log_warn("Certificate verification DISABLED (testing mode)");

    // ===== TEST 1: Basic Echo Test =====
    std::cout << "\n" << CYAN "━━━ TEST 1: Basic Echo Test ━━━" RESET "\n\n";

    // Create TCP connection
    log_info("Creating TCP connection...");
    int fd = create_tcp_connection(host, port);
    if (fd < 0)
    {
        SSL_CTX_free(ctx);
        return 1;
    }
    log_ok("TCP connected (fd=" + std::to_string(fd) + ")");

    // Create SSL object
    SSL* ssl = SSL_new(ctx);
    if (!ssl)
    {
        log_error("Failed to create SSL object: " + get_openssl_error());
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }

    // Set hostname for SNI
    SSL_set_tlsext_host_name(ssl, host);

    // Attach socket to SSL
    if (SSL_set_fd(ssl, fd) != 1)
    {
        log_error("Failed to set SSL fd: " + get_openssl_error());
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }

    // Perform TLS handshake (blocking, userspace)
    log_info("Performing TLS handshake (userspace)...");
    int ret = SSL_connect(ssl);
    if (ret != 1)
    {
        int err = SSL_get_error(ssl, ret);
        log_error("SSL_connect failed: error=" + std::to_string(err) + " " + get_openssl_error());
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }

    // Check KTLS status (should be false)
    bool ktls_active = check_ktls_status(ssl);
    if (ktls_active)
    {
        log_warn("KTLS is unexpectedly ACTIVE! This should not happen.");
    }
    else
    {
        log_ok("KTLS confirmed INACTIVE (using userspace TLS as expected)");
    }

    log_ok(std::string("Handshake complete: ") + SSL_get_version(ssl) + " / " + SSL_get_cipher_name(ssl));

    // Check for pending data after handshake
    int pending = SSL_pending(ssl);
    if (verbose || pending > 0)
    {
        log_info("SSL_pending after handshake: " + std::to_string(pending));
    }

    // Write data using SSL_write (userspace)
    log_info("Writing " + std::to_string(message.size()) + " bytes via SSL_write (userspace)...");
    int written = SSL_write(ssl, message.data(), static_cast<int>(message.size()));
    if (written <= 0)
    {
        int err = SSL_get_error(ssl, written);
        log_error("SSL_write failed: error=" + std::to_string(err) + " " + get_openssl_error());
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }
    log_ok("Wrote " + std::to_string(written) + " bytes");

    // Small delay to simulate network latency
    log_info("Waiting 100ms before reading...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Read response using SSL_read (userspace)
    log_info("Reading response via SSL_read (userspace)...");
    char buffer[4096] = {};
    int bytes_read = SSL_read(ssl, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
    {
        int err = SSL_get_error(ssl, bytes_read);
        if (err == SSL_ERROR_ZERO_RETURN)
        {
            log_warn("Peer closed connection cleanly (no data received)");
        }
        else
        {
            log_error("SSL_read failed: error=" + std::to_string(err) + " errno=" + std::to_string(errno) + " (" + strerror(errno) + ") " + get_openssl_error());
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }

    buffer[bytes_read] = '\0';
    log_ok("Read " + std::to_string(bytes_read) + " bytes: " + std::string(buffer));

    // Verify echo
    if (std::string(buffer) == message)
    {
        std::cout << "\n" GREEN "✅✅✅ ECHO TEST PASSED! ✅✅✅" RESET "\n";
    }
    else
    {
        log_warn("Echo mismatch - sent: '" + message + "' received: '" + std::string(buffer) + "'");
    }

    // Clean shutdown
    log_info("Performing TLS shutdown...");
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);

    // ===== TEST 2: Multiple Messages =====
    std::cout << "\n" << CYAN "━━━ TEST 2: Multiple Messages Test ━━━" RESET "\n\n";

    fd = create_tcp_connection(host, port);
    if (fd < 0)
    {
        SSL_CTX_free(ctx);
        return 1;
    }
    log_ok("TCP connected (fd=" + std::to_string(fd) + ")");

    ssl = SSL_new(ctx);
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set_fd(ssl, fd);

    log_info("Performing TLS handshake...");
    if (SSL_connect(ssl) != 1)
    {
        log_error("SSL_connect failed: " + get_openssl_error());
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }
    log_ok(std::string("Handshake: ") + SSL_get_version(ssl) + " / " + SSL_get_cipher_name(ssl));

    const char* messages[] = {"Message 1: First test\n", "Message 2: Second test\n", "Message 3: Third test\n"};

    bool all_passed = true;
    for (const char* msg: messages)
    {
        size_t msg_len = strlen(msg);
        log_info(std::string("Sending: ") + msg);

        written = SSL_write(ssl, msg, static_cast<int>(msg_len));
        if (written <= 0)
        {
            log_error("SSL_write failed");
            all_passed = false;
            break;
        }

        memset(buffer, 0, sizeof(buffer));
        bytes_read = SSL_read(ssl, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0)
        {
            int err = SSL_get_error(ssl, bytes_read);
            log_error("SSL_read failed: error=" + std::to_string(err) + " errno=" + std::to_string(errno));
            all_passed = false;
            break;
        }

        log_ok(std::string("Received: ") + buffer);

        if (std::string(buffer, bytes_read) != std::string(msg, msg_len))
        {
            log_warn("Echo mismatch!");
            all_passed = false;
        }
    }

    if (all_passed)
    {
        std::cout << "\n" GREEN "✅✅✅ MULTIPLE MESSAGES TEST PASSED! ✅✅✅" RESET "\n";
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);

    // ===== TEST 3: Large Data =====
    std::cout << "\n" << CYAN "━━━ TEST 3: Large Data Test ━━━" RESET "\n\n";

    fd = create_tcp_connection(host, port);
    if (fd < 0)
    {
        SSL_CTX_free(ctx);
        return 1;
    }
    log_ok("TCP connected");

    ssl = SSL_new(ctx);
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set_fd(ssl, fd);

    if (SSL_connect(ssl) != 1)
    {
        log_error("SSL_connect failed");
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return 1;
    }
    log_ok(std::string("Handshake: ") + SSL_get_version(ssl) + " / " + SSL_get_cipher_name(ssl));

    // Send 10KB of data
    const size_t large_size = 10 * 1024;
    std::string large_data(large_size, 'X');
    // Add some variety
    for (size_t i = 0; i < large_size; i += 100)
    {
        large_data[i] = '\n';
    }

    log_info("Sending " + std::to_string(large_size) + " bytes...");

    size_t total_written = 0;
    while (total_written < large_size)
    {
        written = SSL_write(ssl, large_data.data() + total_written, static_cast<int>(large_size - total_written));
        if (written <= 0)
        {
            log_error("SSL_write failed at " + std::to_string(total_written) + " bytes");
            break;
        }
        total_written += written;
    }
    log_ok("Wrote " + std::to_string(total_written) + " bytes");

    // Read back
    std::string received;
    received.reserve(large_size);
    char read_buf[8192];

    log_info("Reading response...");
    while (received.size() < large_size)
    {
        bytes_read = SSL_read(ssl, read_buf, sizeof(read_buf));
        if (bytes_read <= 0)
        {
            int err = SSL_get_error(ssl, bytes_read);
            if (err == SSL_ERROR_ZERO_RETURN)
            {
                log_info("Connection closed after " + std::to_string(received.size()) + " bytes");
            }
            else
            {
                log_error("SSL_read failed: error=" + std::to_string(err));
            }
            break;
        }
        received.append(read_buf, bytes_read);
        if (verbose)
        {
            log_info("Read " + std::to_string(bytes_read) + " bytes (total: " + std::to_string(received.size()) + ")");
        }
    }

    log_ok("Received " + std::to_string(received.size()) + " bytes total");

    if (received == large_data)
    {
        std::cout << "\n" GREEN "✅✅✅ LARGE DATA TEST PASSED! ✅✅✅" RESET "\n";
    }
    else
    {
        log_warn("Data mismatch - sent " + std::to_string(large_size) + ", received " + std::to_string(received.size()));
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);

    // Cleanup
    SSL_CTX_free(ctx);

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    All Tests Complete                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    return 0;
}
