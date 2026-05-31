/* Required before any system header to expose struct ip_mreq on glibc */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <mqueue.h>

/*
 * mc_receiver — Standalone multicast receiver application.
 *
 * Usage: ./mc_receiver <mc_ip> <mc_port>
 *
 * Launched automatically by client_mng via gnome-terminal when a group
 * is joined or created.  Runs in its own terminal window alongside mc_sender.
 *
 * Phases:
 *   1. Parse arguments, report PID to client_mng via message queue.
 *   2. Create a UDP socket, set SO_REUSEADDR, bind to the port,
 *      and join the multicast group with IP_ADD_MEMBERSHIP.
 *   3. Enter the recvfrom loop.  On SIGTERM — drop membership, close
 *      socket, exit cleanly.
 */

/* Queue name is passed as argv[3] — unique per client process */
#define MSG_BUF_SIZE  512

/* ─── Phase 3 helper: SIGTERM handler ──────────────────────────────────────
 *
 * When client_mng calls kill(receiver_pid, SIGTERM), this handler fires.
 * It sets running = 0 and the blocked recvfrom() returns -1 with
 * errno == EINTR (system call interrupted by signal).
 * The main loop detects this and breaks out to do the cleanup.
 * ─────────────────────────────────────────────────────────────────────────── */
static volatile sig_atomic_t running = 1;
static int g_sock = -1;  /* global so SIGTERM handler can close it */

static void sigterm_handler(int sig)
{
    (void)sig;
    running = 0;
    if (g_sock >= 0) close(g_sock);  /* unblock recvfrom() immediately */
}

/* ────────────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <mc_ip> <mc_port> <mq_name>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *mc_ip   = argv[1];
    int         mc_port = atoi(argv[2]);
    const char *mq_name = argv[3];

    if (mc_port <= 0 || mc_port > 65535)
    {
        fprintf(stderr, "mc_receiver: invalid port '%s'\n", argv[2]);
        return EXIT_FAILURE;
    }

    /* ── Phase 1: Report our PID to client_mng via the message queue ────────
     *
     * Identical to mc_sender's Phase 1 — open the queue that client_mng
     * already created, send our PID as raw bytes, then close.
     *
     * client_mng does two mq_receive() calls:
     *   first  → receiver PID  (this process)
     *   second → sender PID    (mc_sender)
     *
     * The order depends on which process sends first.  client_mng must
     * read both PIDs and identify them (e.g. by reading receiver first
     * since it is launched first).
     * ─────────────────────────────────────────────────────────────────────── */
    mqd_t mq = mq_open(mq_name, O_WRONLY);
    if (mq == (mqd_t)-1)
    {
        perror("mc_receiver: mq_open");
        return EXIT_FAILURE;
    }

    pid_t my_pid = getpid();

    if (mq_send(mq, (const char *)&my_pid, sizeof(pid_t), 0) < 0)
    {
        perror("mc_receiver: mq_send");
        mq_close(mq);
        return EXIT_FAILURE;
    }

    mq_close(mq);

    /* ── Phase 2: UDP socket setup ───────────────────────────────────────────
     *
     * Step A — SOCK_DGRAM (UDP):
     *   Multicast uses UDP.  No connection, no handshake.
     *
     * Step B — SO_REUSEADDR:
     *   Allows multiple sockets on the same machine to bind to the same
     *   port.  Without this, only ONE receiver per machine could bind to
     *   port mc_port.  With it, every client on the same host can receive
     *   the same multicast traffic.
     *
     * Step C — bind(INADDR_ANY, mc_port):
     *   We bind to the port on ALL local interfaces, not to the multicast
     *   IP itself.  This is the correct Linux pattern for multicast reception.
     *   Binding to the MC IP directly is unreliable across implementations.
     *   The IP_ADD_MEMBERSHIP below handles which multicast group we join.
     *
     * Step D — IP_ADD_MEMBERSHIP:
     *   This is the key multicast call.  It tells the kernel:
     *     "Deliver UDP packets addressed to mc_ip to my socket."
     *   imr_multiaddr = the multicast group IP  (e.g. 239.0.0.5)
     *   imr_interface = INADDR_ANY = let the OS pick the local interface
     *   Without this call, no multicast packets would ever arrive.
     * ─────────────────────────────────────────────────────────────────────── */

    /* Step A: create UDP socket */
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int sock = g_sock;
    if (sock < 0)
    {
        perror("mc_receiver: socket");
        return EXIT_FAILURE;
    }

    /* Step B: allow multiple receivers on the same port */
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("mc_receiver: setsockopt SO_REUSEADDR");
        close(sock);
        return EXIT_FAILURE;
    }

    /* Step C: bind to the multicast port on all interfaces */
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family      = AF_INET;
    local_addr.sin_port        = htons((uint16_t)mc_port);
    local_addr.sin_addr.s_addr = INADDR_ANY;  /* all interfaces */

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        perror("mc_receiver: bind");
        close(sock);
        return EXIT_FAILURE;
    }

    /* Step D: join the multicast group */
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_interface.s_addr = INADDR_ANY;  /* use the default interface */

    if (inet_pton(AF_INET, mc_ip, &mreq.imr_multiaddr) <= 0)
    {
        fprintf(stderr, "mc_receiver: bad multicast IP '%s'\n", mc_ip);
        close(sock);
        return EXIT_FAILURE;
    }

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        perror("mc_receiver: setsockopt IP_ADD_MEMBERSHIP");
        close(sock);
        return EXIT_FAILURE;
    }

    /* ── Phase 3: SIGTERM handler + recvfrom loop ────────────────────────────
     *
     * Register the signal handler before entering the loop.
     *
     * recvfrom() blocks until a UDP packet arrives on this socket.
     * When a packet arrives we null-terminate the buffer and print it.
     * fflush(stdout) ensures the line appears immediately in the terminal
     * rather than waiting for the output buffer to fill.
     *
     * Loop exit conditions:
     *   a) SIGTERM received → handler sets running = 0 AND recvfrom()
     *      returns -1 with errno == EINTR (interrupted system call).
     *      We check both: errno == EINTR OR running == 0.
     *   b) recvfrom() returns another error → print and break.
     *
     * Cleanup after the loop (IP_DROP_MEMBERSHIP):
     *   Politely tells the kernel to stop delivering this multicast
     *   group's packets to our socket.  Good practice even on exit —
     *   on some systems it speeds up the kernel's group tracking cleanup.
     * ─────────────────────────────────────────────────────────────────────── */
    signal(SIGTERM, sigterm_handler);

    char buf[MSG_BUF_SIZE];

    printf("[mc_receiver] Listening on %s:%d\n\n", mc_ip, mc_port);

    while (running)
    {
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);

        if (n < 0)
        {
            /* EINTR means the system call was interrupted by our signal —
             * this is the normal SIGTERM exit path, not an error.          */
            if (errno == EINTR) break;

            perror("mc_receiver: recvfrom");
            break;
        }

        /* Null-terminate so we can safely print the message as a C string */
        buf[n] = '\0';

        printf("%s\n", buf);
        fflush(stdout);  /* flush immediately — terminal output is line-buffered */
    }

    /* ── Cleanup: leave the multicast group then close the socket ─────────── */
    if (setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        perror("mc_receiver: setsockopt IP_DROP_MEMBERSHIP");

    close(sock);
    printf("\n[mc_receiver] Left group %s. Exiting.\n", mc_ip);
    return EXIT_SUCCESS;
}