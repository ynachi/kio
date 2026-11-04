/* Timed-wait canary: does the EXT_ARG deadline fire on a DEFER_TASKRUN ring?
 *
 * Q1: io_uring_submit_and_wait_timeout with a 50ms deadline, NOTHING armed:
 *     expect rc == -ETIME after ~50ms (kStopPoll idle-tick contract).
 * Q2: same, with a pending recv that completes via deferred task work
 *     mid-wait: expect rc >= 0 (or 1 submitted) early, CQE visible.
 * Q3: plain io_uring_wait_cqes with the same deadline, nothing armed:
 *     expect -ETIME after ~50ms (the split-wait alternative).
 *
 * Run under `timeout 10` -- a hang IS the answer (deadline never fires).
 */
#define _GNU_SOURCE
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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

    struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 50000000};

    /* Q1: idle ring, deadline only. */
    double t0 = now_ms();
    struct io_uring_cqe *cqe = NULL;
    rc = io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &ts, NULL);
    printf("Q1 idle wait_timeout: rc=%d (%s) after %.1fms\n", rc, strerror(-rc < 0 ? -rc : 0),
           now_ms() - t0);

    /* Q2: pending recv completed by task work DURING the wait. */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    unsigned char buf[64];
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv(sqe, sv[0], buf, sizeof buf, 0);
    io_uring_sqe_set_data64(sqe, 0xBEEF);
    char msg = 'x';
    (void)write(sv[1], &msg, 1); /* completion becomes deferred task work */
    t0 = now_ms();
    rc = io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &ts, NULL);
    printf("Q2 recv wait_timeout: rc=%d cqe.res=%d after %.1fms\n", rc,
           cqe ? cqe->res : -1, now_ms() - t0);
    if (cqe)
    {
        io_uring_cqe_seen(&ring, cqe);
    }
    close(sv[0]);
    close(sv[1]);

    /* Q3: split form -- plain wait_cqes with the deadline, idle ring. */
    t0 = now_ms();
    cqe = NULL;
    rc = io_uring_wait_cqes(&ring, &cqe, 1, &ts, NULL);
    printf("Q3 idle wait_cqes:    rc=%d (%s) after %.1fms\n", rc, strerror(-rc < 0 ? -rc : 0),
           now_ms() - t0);

    io_uring_queue_exit(&ring);
    return 0;
}
