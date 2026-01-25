// examples/10_chat_server.cpp
// Demonstrates: Shared state, broadcasting, TaskGroup lifetime management

#include <array>
#include <memory>
#include <mutex>
#include <print>
#include <set>
#include <string>

#include "aio/io.hpp"
#include "aio/io_context.hpp"
#include "aio/net.hpp"
#include "aio/task.hpp"
#include "aio/task_group.hpp"

class ChatRoom
{
public:
    void Join(int fd)
    {
        std::scoped_lock lock(mutex_);
        clients_.insert(fd);
        std::println("[Room] Client {} joined ({} total)", fd, clients_.size());
    }

    void Leave(int fd)
    {
        std::scoped_lock lock(mutex_);
        clients_.erase(fd);
        std::println("[Room] Client {} left ({} total)", fd, clients_.size());
    }

    std::vector<int> GetClients() const
    {
        std::scoped_lock lock(mutex_);
        return {clients_.begin(), clients_.end()};
    }

private:
    mutable std::mutex mutex_;
    std::set<int> clients_;
};

aio::Task<> broadcast(aio::IoContext& ctx, ChatRoom& room,
                      int sender_fd, std::string_view message)
{
    auto clients = room.GetClients();

    for (int fd : clients)
    {
        if (fd != sender_fd)
        {
            // Best effort send (ignore errors for simplicity)
            co_await aio::AsyncSend(ctx, fd, message);
        }
    }
}

aio::Task<> handle_client(aio::IoContext& ctx, ChatRoom& room, int fd)
{
    room.Join(fd);

    // Send welcome message
    co_await aio::AsyncSend(ctx, fd, std::string_view{"Welcome to the chat!\n"});

    std::array<std::byte, 512> buffer{};

    while (true)
    {
        auto recv_result = co_await aio::AsyncRecv(ctx, fd, buffer);
        if (!recv_result || *recv_result == 0)
            break;

        // Format message with sender id
        std::string msg = std::format("[Client {}]: {}", fd,
            std::string_view{reinterpret_cast<char*>(buffer.data()), *recv_result});

        std::print("{}", msg);  // Log to server console

        // Broadcast to other clients
        co_await broadcast(ctx, room, fd, msg);
    }

    room.Leave(fd);
    co_await aio::AsyncClose(ctx, fd);
}

aio::Task<> server(aio::IoContext& ctx, uint16_t port)
{
    auto listener = aio::net::TcpListener::Bind(port);
    if (!listener)
    {
        std::println(stderr, "Failed to bind to port {}", port);
        co_return;
    }

    std::println("Chat server listening on port {}", port);
    std::println("Connect with: nc localhost {}\n", port);

    ChatRoom room;
    aio::TaskGroup clients;

    while (true)
    {
        auto accept_result = co_await aio::AsyncAccept(ctx, listener->Get());
        if (!accept_result)
            continue;

        clients.Spawn(handle_client(ctx, room, accept_result->fd));

        if (clients.Size() > 100)
            clients.Sweep();
    }
}

int main(int argc, char** argv)
{
    uint16_t port = (argc >= 2) ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;

    aio::IoContext ctx;

    auto task = server(ctx, port);
    ctx.RunUntilDone(task);

    return 0;
}