//
// Created by Yao ACHI on 06/10/2025.
//

#ifndef KIO_NET_H
#define KIO_NET_H
#include <cstdint>
#include <string_view>

namespace kio::net
{
    int create_socket(std::string_view ip_address, std::uint16_t port);
    void listen_on_sock(int fd);
    // set so_reuse on sockets
    void set_fd_server_options(int fd);
}  // namespace kio::net

#endif  // KIO_NET_H
