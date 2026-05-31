#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <mqueue.h>

/*
 * mc_sender — Standalone multicast sender application.
 *
 * Usage: ./mc_sender <mc_ip> <mc_port> <username>
 *
 * Launched automatically by client_mng via gnome-terminal when a group
 * is joined or created.  Runs in its own terminal window.
 *
 * Phases:
 *   1. Parse arguments, report PID to client_mng via message queue.
 *   2. Create a UDP socket and set the multicast TTL.
 *   3. Enter the read-send loop.  On SIGTERM — exit cleanly.
 */

/* Queue name is passed as argv[4] — unique per client process */
#define MSG_BUF_SIZE  512             /* max bytes a user can type per line             */

/* ─── Phase 3 helper: SIGTERM handler ──────────────────────────────────────
 *
 * `volatile sig_atomic_t` is the only type guaranteed safe to write from
 * a signal handler and read from the main loop without a data race.
 * Setting running = 0 causes the while loop to exit after the current
 * fgets() returns (which it does when the signal interrupts it).
 * ─────────────────────────────────────────────────────────────────────────── */
static volatile sig_atomic_t running = 1;

static void sigterm_handler(int sig)
{
    (void)sig;
    running = 0;
    close(STDIN_FILENO);  /* unblock fgets() immediately so the loop exits */
}

/* ────────────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <mc_ip> <mc_port> <username> <mq_name>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *mc_ip    = argv[1];
    int         mc_port  = atoi(argv[2]);
    const char *username = argv[3];
    const char *mq_name  = argv[4];

    if (mc_port <= 0 || mc_port > 65535)
    {
        fprintf(stderr, "mc_sender: invalid port '%s'\n", argv[2]);
        return EXIT_FAILURE;
    }

    /* ── Phase 1: Report our PID to client_mng via the message queue ────────
     *
     * client_mng creates the queue BEFORE launching this process.
     * We open it with O_WRONLY (we only send — client_mng only reads).
     *
     * getpid() returns our own PID.  We send it as raw bytes (sizeof(pid_t)).
     * client_mng does mq_receive() twice — once for the receiver PID and
     * once for our PID — and stores both in the GroupEntry.
     *
     * We close the queue immediately after sending — we no longer need it.
     * ─────────────────────────────────────────────────────────────────────── */
    mqd_t mq = mq_open(mq_name, O_WRONLY);
    if (mq == (mqd_t)-1)
    {
        perror("mc_sender: mq_open");
        return EXIT_FAILURE;
    }

    pid_t my_pid = getpid();

    if (mq_send(mq, (const char *)&my_pid, sizeof(pid_t), 0) < 0)
    {
        perror("mc_sender: mq_send");
        mq_close(mq);
        return EXIT_FAILURE;
    }

    mq_close(mq);

    /* ── Phase 2: UDP socket + multicast TTL ─────────────────────────────────
     *
     * SOCK_DGRAM = UDP.  We do NOT use SOCK_STREAM (TCP) here —
     * multicast is always UDP.
     *
     * IP_MULTICAST_TTL = 1:
     *   TTL (Time To Live) is decremented by each router the packet crosses.
     *   Setting it to 1 means the packet is discarded after the first router —
     *   it never leaves the LAN.  This is the correct value for local chat.
     *
     * No bind() on the sender:
     *   The sender does not need to bind to a port.  We just call sendto()
     *   directly with the multicast destination address.  The OS picks
     *   the source port and outgoing interface automatically.
     * ─────────────────────────────────────────────────────────────────────── */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("mc_sender: socket");
        return EXIT_FAILURE;
    }

    unsigned char ttl = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
    {
        perror("mc_sender: setsockopt IP_MULTICAST_TTL");
        close(sock);
        return EXIT_FAILURE;
    }

    /* Fill the destination address structure — used in every sendto() call */
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port   = htons((uint16_t)mc_port);

    if (inet_pton(AF_INET, mc_ip, &dest_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "mc_sender: bad multicast IP '%s'\n", mc_ip);
        close(sock);
        return EXIT_FAILURE;
    }

    /* ── Phase 3: SIGTERM handler + read-send loop ───────────────────────────
     *
     * Register the SIGTERM handler.
     * client_mng calls kill(sender_pid, SIGTERM) when the user leaves the
     * group or logs out.  The handler sets running = 0, which causes the
     * while condition to fail and the program to exit cleanly.
     *
     * fgets() on the blocked stdin will return NULL when interrupted by
     * a signal, which also exits the loop.
     *
     * Loop body:
     *   1. fgets() — blocks until the user presses Enter.
     *   2. Strip the trailing newline that fgets() includes.
     *   3. snprintf — prepend "username: " to the typed line.
     *   4. sendto() — deliver the message to the multicast group.
     *      Every receiver that has joined this MC IP will get it.
     * ─────────────────────────────────────────────────────────────────────── */
    signal(SIGTERM, sigterm_handler);

    char line[MSG_BUF_SIZE];
    char message[MSG_BUF_SIZE + 64]; /* extra room for "username: " prefix */

    printf("[mc_sender] Ready. Type a message and press Enter.\n");
    printf("[mc_sender] Sending to %s:%d as '%s'\n\n", mc_ip, mc_port, username);

    while (running && fgets(line, sizeof(line), stdin) != NULL)
    {
        /* Strip the trailing newline fgets() always includes */
        line[strcspn(line, "\n")] = '\0';

        if (line[0] == '\0') continue; /* skip empty lines */

        /* Format: "alice: hello everyone" */
        snprintf(message, sizeof(message), "%s: %s", username, line);

        /* sendto() — send to the multicast group address.
         * All receivers that joined this MC IP will receive this datagram. */
        if (sendto(sock, message, strlen(message), 0,
                   (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
        {
            perror("mc_sender: sendto");
        }
    }

    close(sock);
    printf("\n[mc_sender] Exiting.\n");
    return EXIT_SUCCESS;
}