/* DEFER_TASKRUN starvation canary.
 *
 * Q1: after a recv completes via task work, does plain io_uring_submit
 *     (empty SQ -> no enter at all) post the CQE?
 * Q2: does a plain submit WITH SQEs (enter without GETEVENTS) post it?
 * Q3: does io_uring_submit_and_get_events post it?
 */
#define _GNU_SOURCE
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned drain(struct io_uring *ring)
{
    struct io_uring_cqe *cqe;
    unsigned n = 0;
    while (io_uring_peek_cqe(ring, &cqe) == 0 && cqe)
    {
        printf("    cqe: res=%d user_data=%llu\n", cqe->res,
               (unsigned long long)io_uring_cqe_get_data64(cqe));
        io_uring_cqe_seen(ring, cqe);
        n++;
    }
    return n;
}

int main(void)
{
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

    struct io_uring ring;
    int rc = io_uring_queue_init_params(8, &ring, &p);
    if (rc < 0)
    {
        printf("queue_init failed: %s\n", strerror(-rc));
        return 1;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    {
        perror("socketpair");
        return 1;
    }

    unsigned char buf[64];

    /* 1. Arm a recv on sv[0]: submit with no data yet, so it pends. */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv(sqe, sv[0], buf, sizeof buf, 0);
    io_uring_sqe_set_data64(sqe, 0xA11CE);
    io_uring_submit(&ring);
    printf("armed recv; cq_ready=%u\n", io_uring_cq_ready(&ring));

    /* 2. Complete it from this thread: the wake becomes deferred task work. */
    char msg = 'x';
    (void)write(sv[1], &msg, 1);
    printf("after write:   cq_ready=%u   (0 => completion is deferred task work)\n",
           io_uring_cq_ready(&ring));

    /* Q1: plain submit with EMPTY SQ -- expect no enter, no CQE. */
    io_uring_submit(&ring);
    printf("Q1 empty-SQ plain submit:      cq_ready=%u\n", io_uring_cq_ready(&ring));
    drain(&ring);

    /* Q2: plain submit WITH a nop SQE -- enter without GETEVENTS. */
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 0x0B0B);
    io_uring_submit(&ring);
    printf("Q2 submit+nop (no GETEVENTS): cq_ready=%u   (1 => only nop; 2 => task work ran)\n",
           io_uring_cq_ready(&ring));
    drain(&ring);

    /* Q3: submit_and_get_events -- GETEVENTS with waitnr 0. */
    rc = io_uring_submit_and_get_events(&ring);
    printf("Q3 submit_and_get_events:     rc=%d cq_ready=%u\n", rc, io_uring_cq_ready(&ring));
    unsigned got = drain(&ring);

    printf("VERDICT: recv cqe arrived at %s\n",
           got ? "Q3 (submit_and_get_events)" : "never (check earlier output)");

    io_uring_queue_exit(&ring);
    close(sv[0]);
    close(sv[1]);
    return 0;
}
