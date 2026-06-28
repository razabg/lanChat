
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ds/genDList.h>
#include "server_net.h"
#include "../common/protocol.h"

#define RECV_BUF_SIZE 4096

/* ─── Per-client state node ─────────────────────────────────────────────── */
typedef struct ClientNode {
    int     socket_fd;
    int     client_id;
    uint8_t recv_buf[RECV_BUF_SIZE];
    int     buf_len;
} ClientNode;

/* ─── Manager struct (opaque outside this file) ─────────────────────────── */
struct ServerNet {
    int            listen_fd;
    int            running;
    int            next_id;
    List          *clients;
    OnMessageCb    on_message;
    OnDisconnectCb on_disconnect;
};

/* ─── Forward declarations for static helpers ──────────────────────────── */
static int  build_fd_set(ServerNet *net, fd_set *read_fds);
static void accept_new_client(ServerNet *net);
static void handle_client_data(ServerNet *net, ClientNode *node);
static void process_all_clients(ServerNet *net, fd_set *read_fds);

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ═══════════════════════════════════════════════════════════════════════════ */

ServerNet *ServerNet_Create(int port, OnMessageCb on_msg, OnDisconnectCb on_disc)
{
    ServerNet *net = malloc(sizeof(ServerNet));
    if (!net)
    {
        return NULL;
    }

    net->clients = ListCreate();
    if (!net->clients)
    {
        free(net);
        return NULL;
    }

    struct sockaddr_in addr;
    net->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (net->listen_fd < 0)
    {
        perror("socket");
        ListDestroy(&net->clients, NULL);
        free(net);
        return NULL;
    }

    int opt = 1;
    setsockopt(net->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(net->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(net->listen_fd);
        ListDestroy(&net->clients, NULL);
        free(net);
        return NULL;
    }

    if (listen(net->listen_fd, 10) < 0)
    {
        perror("listen");
        close(net->listen_fd);
        ListDestroy(&net->clients, NULL);
        free(net);
        return NULL;
    }

    net->on_message    = on_msg;
    net->on_disconnect = on_disc;
    net->next_id       = 1;
    net->running       = 0;

    return net;
}

void ServerNet_Run(ServerNet *net)
{
    net->running = 1;

    while (net->running)
    {
        fd_set read_fds;
        int max_fd = build_fd_set(net, &read_fds);

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0)
        {
            perror("select");
            break;
        }

        if (FD_ISSET(net->listen_fd, &read_fds))
        {
            accept_new_client(net);
        }

        process_all_clients(net, &read_fds);
    }
}

void ServerNet_StopRun(ServerNet *net)
{
    net->running = 0;
}

ServerNetResult ServerNet_SendMsg(ServerNet *net, int client_id, const uint8_t *msg, int msg_len)
{
    ListItr itr = ListItrBegin(net->clients);
    ListItr end = ListItrEnd(net->clients);

    while (itr != end)
    {
        ClientNode *node = (ClientNode *)ListItrGet(itr);
        if (node->client_id == client_id)
        {
            int sent = send(node->socket_fd, msg, msg_len, 0);
            return (sent == msg_len) ? SERVER_NET_SUCCESS : SERVER_NET_SEND_ERROR;
        }
        itr = ListItrNext(itr);
    }

    return SERVER_NET_CLIENT_NOT_FOUND;
}

void ServerNet_Destroy(ServerNet **net)
{
    if (!net || !*net)
    {
        return;
    }

    ListItr itr = ListItrBegin((*net)->clients);
    ListItr end = ListItrEnd((*net)->clients);

    while (itr != end)
    {
        ClientNode *node = (ClientNode *)ListItrGet(itr);
        close(node->socket_fd);
        free(node);
        itr = ListItrNext(itr);
    }

    ListDestroy(&(*net)->clients, NULL);
    close((*net)->listen_fd);
    free(*net);
    *net = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
   STATIC HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

/* Adds the listening socket + all client sockets to read_fds.
   Returns the highest fd value (needed by select). */
static int build_fd_set(ServerNet *net, fd_set *read_fds)
{
    FD_ZERO(read_fds);
    FD_SET(net->listen_fd, read_fds);
    int max_fd = net->listen_fd;

    ListItr itr = ListItrBegin(net->clients);
    ListItr end = ListItrEnd(net->clients);
    while (itr != end)
    {
        ClientNode *node = (ClientNode *)ListItrGet(itr);
        FD_SET(node->socket_fd, read_fds);
        if (node->socket_fd > max_fd)
        {
            max_fd = node->socket_fd;
        }
        itr = ListItrNext(itr);
    }

    return max_fd;
}

/* Accepts a new incoming connection and adds it to the client list. */
static void accept_new_client(ServerNet *net)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(net->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0)
    {
        return;
    }

    ClientNode *node = malloc(sizeof(ClientNode));
    if (!node)
    {
        close(client_fd);
        return;
    }

    node->socket_fd = client_fd;
    node->client_id = net->next_id++;
    node->buf_len   = 0;
    memset(node->recv_buf, 0, RECV_BUF_SIZE);
    ListPushHead(net->clients, node);
}

/* Parses complete TLVs out of the client's buffer and fires the message callback.
   Removes consumed bytes from the front of the buffer after each message. */
static void handle_client_data(ServerNet *net, ClientNode *node)
{
    while (node->buf_len >= TLV_HEADER_SIZE)
    {
        uint16_t msg_len;
        memcpy(&msg_len, node->recv_buf + TLV_TAG_SIZE, TLV_LENGTH_SIZE);
        msg_len = ntohs(msg_len);
        int total = TLV_HEADER_SIZE + msg_len;

        if (node->buf_len < total)
        {
            break;
        }

        net->on_message(node->client_id, node->recv_buf, total);
        memmove(node->recv_buf, node->recv_buf + total, node->buf_len - total);
        node->buf_len -= total;
    }
}

/* Iterates all clients, reads data from those flagged in read_fds.
   Handles disconnect and passes received bytes to handle_client_data. */
static void process_all_clients(ServerNet *net, fd_set *read_fds)
{
    ListItr itr = ListItrBegin(net->clients);
    ListItr end = ListItrEnd(net->clients);

    while (itr != end)
    {
        ListItr current = itr;
        itr = ListItrNext(itr);
        ClientNode *node = (ClientNode *)ListItrGet(current);

        if (!FD_ISSET(node->socket_fd, read_fds))
        {
            continue;
        }

        int bytes = recv(node->socket_fd,
                         node->recv_buf + node->buf_len,
                         RECV_BUF_SIZE - node->buf_len, 0);

        if (bytes <= 0)
        {
            net->on_disconnect(node->client_id);
            close(node->socket_fd);
            ListItrRemove(current);
            free(node);
            continue;
        }

        node->buf_len += bytes;
        handle_client_data(net, node);
    }
}
