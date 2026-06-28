#ifndef SERVER_NET_H
#define SERVER_NET_H

#include <stdint.h>

/* ─── Opaque manager type ───────────────────────────────────────────────── */
typedef struct ServerNet ServerNet;

/* ─── Result codes ──────────────────────────────────────────────────────── */
typedef enum
{
    SERVER_NET_SUCCESS,
    SERVER_NET_UNINITIALIZED_ERROR,
    SERVER_NET_ALLOCATION_ERROR,
    SERVER_NET_SOCKET_ERROR,
    SERVER_NET_CLIENT_NOT_FOUND,
    SERVER_NET_SEND_ERROR
} ServerNetResult;

typedef void (*OnMessageCb)(int client_id, const uint8_t *msg, int msg_len);
typedef void (*OnDisconnectCb)(int client_id);

//************ ServerNet API *************

/**
 * @brief Initialize the server: create TCP socket, bind, and listen
 *
 * @param port    : port number to listen on
 * @param on_msg  : callback fired when a complete TLV message arrives from a client
 * @param on_disc : callback fired when a client disconnects
 * @return ServerNet* on success, NULL on failure
 */
ServerNet *ServerNet_Create(int port, OnMessageCb on_msg, OnDisconnectCb on_disc);

/**
 * @brief Enter the main select() event loop — blocks until StopRun is called
 *
 * @param net : the server net manager
 */
void ServerNet_Run(ServerNet *net);

/**
 * @brief Signal the event loop to stop on the next iteration
 *
 * @param net : the server net manager
 */
void ServerNet_StopRun(ServerNet *net);

/**
 * @brief Send a message to a specific connected client
 *
 * @param net       : the server net manager
 * @param client_id : the ID of the target client
 * @param msg       : buffer containing the TLV bytes to send
 * @param msg_len   : number of bytes to send
 * @return SERVER_NET_SUCCESS, SERVER_NET_CLIENT_NOT_FOUND, or SERVER_NET_SEND_ERROR
 */
ServerNetResult ServerNet_SendMsg(ServerNet *net, int client_id, const uint8_t *msg, int msg_len);

/**
 * @brief Close all client connections, free all resources
 *
 * @param net : pointer to the manager pointer (set to NULL on destroy)
 */
void ServerNet_Destroy(ServerNet **net);

#endif /* SERVER_NET_H */
