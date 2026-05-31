#ifndef CLIENT_NET_H
#define CLIENT_NET_H

#include <stdint.h>
#define RECV_BUFFER_SIZE 1024

/* Holds the TCP socket and the partial-TLV receive buffer */
typedef struct
{
    int      sockfd;
    uint8_t  recv_buf[RECV_BUFFER_SIZE];
    int      buf_len;   /* bytes currently in recv_buf */
} ClientNet;

/*
 * ClientNet_Create
 * ----------------
 * Creates a TCP socket and connects to the server at server_ip:port.
 * Initialises the receive buffer to empty.
 *
 * Returns a heap-allocated ClientNet on success, NULL on error.
 */
ClientNet *ClientNet_Create(const char *server_ip, uint16_t port);

/*
 * ClientNet_SendMsg
 * -----------------
 * Sends a complete TLV message buffer to the server.
 * buf  : pointer to the encoded TLV bytes.
 * len  : number of bytes to send.
 *
 * Returns 0 on success, -1 on error.
 */
int ClientNet_SendMsg(ClientNet *net, const uint8_t *buf, uint16_t len);

/*
 * ClientNet_RecvMsg
 * -----------------
 * Reads available bytes from the socket into the internal receive buffer.
 * When a complete TLV (tag + 2-byte length + value) is assembled it is
 * copied into out_buf and removed from the internal buffer.
 *
 * out_buf  : caller-supplied buffer to receive the complete TLV.
 * out_size : capacity of out_buf in bytes.
 *
 * Returns the total byte length of the TLV placed in out_buf,
 *         0  if no complete TLV is available yet (need more data),
 *        -1  on socket error or connection closed.
 */
int ClientNet_RecvMsg(ClientNet *net, uint8_t *out_buf, int out_size);
/*
 * ClientNet_Destroy
 * -----------------
 * Closes the TCP socket and frees the ClientNet struct.
 */
void ClientNet_Destroy(ClientNet *net);

#endif /* CLIENT_NET_H */