// Canary: recv + IOSQE_IO_LINK + link_timeout on a blocking socketpair end,
// exactly as IoAwaitable::await_suspend arms it (DEFER_TASKRUN ring).
// Prints every CQE (user_data, res) so the accounting is visible.
#include <liburing.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

static int g_writer_fd = -1;

static void* writer(void*)
{
    usleep(200 * 1000);  // 200ms: well before the 1s link timeout
    const char c = 'x';
    ssize_t n = ::write(g_writer_fd, &c, 1);
    printf("[writer] wrote %zd\n", n);
    return nullptr;
}

int main()
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;
    g_writer_fd = sv[1];

    io_uring ring;
    io_uring_params p{};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    if (int r = io_uring_queue_init_params(64, &ring, &p); r < 0)
    {
        printf("ring init failed: %d\n", r);
        return 1;
    }
    io_uring_register(ring.ring_fd, IORING_REGISTER_ENABLE_RINGS, nullptr, 0);

    constexpr std::uint64_t OP_TAG = 0xDEAD;
    constexpr std::uint64_t IGNORE = 1;

    // recv SQE with LINK, then link timeout (1s), as await_suspend does
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    std::byte b{};
    io_uring_prep_recv(sqe, sv[0], &b, 1, 0);
    io_uring_sqe_set_data64(sqe, OP_TAG);
    sqe->flags |= IOSQE_IO_LINK;

    __kernel_timespec ts{.tv_sec = 1, .tv_nsec = 0};
    io_uring_sqe* t = io_uring_get_sqe(&ring);
    io_uring_prep_link_timeout(t, &ts, 0);
    io_uring_sqe_set_data64(t, IGNORE);

    pthread_t th;
    pthread_create(&th, nullptr, writer, nullptr);

    // enter with GETEVENTS repeatedly for up to 3s, print all CQEs
    for (int i = 0; i < 300; ++i)
    {
        int r = io_uring_submit_and_get_events(&ring);
        if (r < 0 && r != -EINTR && r != -EBUSY)
        {
            printf("enter failed: %d\n", r);
            return 1;
        }
        io_uring_cqe* cqe;
        unsigned head;
        unsigned n = 0;
        io_uring_for_each_cqe(&ring, head, cqe)
        {
            ++n;
            printf("[cqe] user_data=%#lx res=%d\n",
                   static_cast<unsigned long>(io_uring_cqe_get_data64(cqe)), cqe->res);
        }
        io_uring_cq_advance(&ring, n);
        if (n > 0 && i > 250) break;
        usleep(10 * 1000);
    }
    pthread_join(th, nullptr);
    io_uring_queue_exit(&ring);
    printf("done\n");
    return 0;
}
