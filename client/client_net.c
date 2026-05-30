#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "client_net.h"

/* ─── Phase 1: ClientNet_Create ─────────────────────────────────────────────
 *
 * Goal: allocate the ClientNet struct, open a TCP socket, and connect to the
 * server so that every later Send/Recv call has a live connection to use.
 *
 * Steps:
 *   1. malloc the struct.
 *   2. socket() — asks the OS for a TCP file descriptor.
 *   3. inet_pton() — converts the human-readable IP string ("192.168.1.5")
 *      into the 4-byte binary form that sockaddr_in expects.
 *   4. htons()   — converts the port from host byte order to network byte
 *      order (big-endian), as required by the socket API.
 *   5. connect() — performs the TCP three-way handshake with the server.
 *   6. Zero buf_len so the receive buffer is treated as empty.
 *
 * On any failure we free whatever was already allocated and return NULL,
 * letting the caller decide whether to retry or abort.
 * ─────────────────────────────────────────────────────────────────────────── */
ClientNet *ClientNet_Create(const char *server_ip, uint16_t port)
{
    ClientNet *net = malloc(sizeof(ClientNet));
    if (!net) {
        perror("ClientNet_Create: malloc");
        return NULL;
    }

    net->sockfd  = socket(AF_INET, SOCK_STREAM, 0);
    if (net->sockfd < 0) {
        perror("ClientNet_Create: socket");
        free(net);
        return NULL;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("ClientNet_Create: inet_pton (bad IP address?)");
        close(net->sockfd);
        free(net);
        return NULL;
    }
    //performs the TCP three-way handshake with the server
    if (connect(net->sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("ClientNet_Create: connect");
        close(net->sockfd);
        free(net);
        return NULL;
    }
    //Zero buf_len so the receive buffer is treated as empty
    net->buf_len = 0;
    return net;
}

/* ─── Phase 2: ClientNet_SendMsg ────────────────────────────────────────────
 *
 * Goal: deliver every byte of the TLV buffer to the server, reliably.
 *
 * Why a loop?
 *   send() is not guaranteed to transmit all bytes in one call — it returns
 *   how many it actually sent, which can be less than `len` when the kernel
 *   send buffer is temporarily full.  If we called send() only once and
 *   ignored a short write, we would silently truncate the message and the
 *   server would receive malformed or partial TLV data.
 *
 * The loop keeps advancing the pointer (buf + sent) and decrementing the
 * remaining byte count until every byte is delivered or an error occurs.
 * ─────────────────────────────────────────────────────────────────────────── */
int ClientNet_SendMsg(ClientNet *net, const uint8_t *buf, uint16_t len)
{
    uint16_t sent = 0;

    while (sent < len) {
        ssize_t n = send(net->sockfd, buf + sent, len - sent, 0);
        if (n <= 0) {
            perror("ClientNet_SendMsg: send");
            return -1;
        }
        sent += (uint16_t)n;
    }

    return 0;
}

/* ─── Phase 3: ClientNet_RecvMsg ────────────────────────────────────────────
 *
 * Goal: read raw bytes from the socket, accumulate them in an internal
 * buffer, and return exactly one complete TLV to the caller when enough
 * bytes have arrived.
 *
 * Why a buffer?
 *   TCP is a byte stream — there are no message boundaries.  A 20-byte TLV
 *   might arrive as two recv() calls of 8 + 12 bytes, or it might arrive
 *   together with the first 3 bytes of the next message.  We must buffer
 *   partial data and wait until we have the full TLV before handing it up.
 *
 * TLV wire layout (from protocol.h):
 *   [ tag: 1 byte ][ length: 2 bytes, big-endian ][ value: length bytes ]
 *   header size = 3 bytes
 *
 * Algorithm:
 *   Step A — recv() into the tail of recv_buf, appending new bytes.
 *             Return -1 if the connection was closed (n == 0) or errored.
 *
 *   Step B — Do we have at least 3 bytes (the header)?
 *             If not, return 0 (caller must call us again later).
 *
 *   Step C — Read the 2-byte length field at offset 1.
 *             ntohs() converts big-endian network order → host order.
 *             Compute total_tlv_len = 3 + value_len.
 *
 *   Step D — Do we have all total_tlv_len bytes in the buffer?
 *             If not, return 0 (still waiting for more data).
 *
 *   Step E — We have a complete TLV.  Copy it into out_buf.
 *             Then shift the remaining bytes in recv_buf to the front
 *             (memmove left by total_tlv_len), decrement buf_len.
 *             Return total_tlv_len so the caller knows how many bytes
 *             are in out_buf.
 * ─────────────────────────────────────────────────────────────────────────── */
int ClientNet_RecvMsg(ClientNet *net, uint8_t *out_buf, int out_size)
{
    /* ── Step A: read new bytes from the socket into the tail of recv_buf ── */
    int space = RECV_BUFFER_SIZE - net->buf_len;
    if (space > 0) {
        ssize_t n = recv(net->sockfd,
                         net->recv_buf + net->buf_len,
                         (size_t)space,
                         0);
        if (n < 0) {
            perror("ClientNet_RecvMsg: recv");
            return -1;
        }
        if (n == 0) {
            /* Server closed the connection */
            return -1;
        }
        net->buf_len += (int)n;
    }

    /* ── Step B: need at least the 3-byte TLV header ── */
    if (net->buf_len < 3) {
        return 0;
    }

    /* ── Step C: read the 2-byte length field (big-endian) ── */
    uint16_t value_len;
    memcpy(&value_len, net->recv_buf + 1, sizeof(uint16_t));
    value_len = ntohs(value_len);

    int total_tlv_len = 3 + (int)value_len;

    /* ── Step D: do we have the full TLV yet? ── */
    if (net->buf_len < total_tlv_len) {
        return 0;
    }

    /* ── Step E: complete TLV available — copy it out then consume it ── */
    if (total_tlv_len > out_size) {
        /* Caller's buffer is too small; skip this TLV to avoid a stall */
        fprintf(stderr, "ClientNet_RecvMsg: out_buf too small (%d < %d)\n",
                out_size, total_tlv_len);
        memmove(net->recv_buf,
                net->recv_buf + total_tlv_len,
                (size_t)(net->buf_len - total_tlv_len));
        net->buf_len -= total_tlv_len;
        return -1;
    }

    memcpy(out_buf, net->recv_buf, (size_t)total_tlv_len);

    memmove(net->recv_buf,
            net->recv_buf + total_tlv_len,
            (size_t)(net->buf_len - total_tlv_len));
    net->buf_len -= total_tlv_len;

    return total_tlv_len;
}

/* ─── Phase 4: ClientNet_Destroy ────────────────────────────────────────────
 *
 * Goal: release every resource owned by ClientNet.
 *
 * close(sockfd) — tells the OS to send a TCP FIN to the server (graceful
 *   shutdown) and reclaim the file descriptor.
 * free(net)     — returns the heap allocation.
 *
 * We guard against a NULL pointer so callers can safely call Destroy even
 * when Create failed partway through and returned NULL.
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientNet_Destroy(ClientNet *net)
{
    if (!net){
        return;
    } 
    close(net->sockfd);
    free(net);
}