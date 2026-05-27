#ifndef SERVER_NET_H
#define SERVER_NET_H

#include <stdint.h>

typedef void (*OnMessageCb)(int client_id, const uint8_t *msg, int msg_len);
typedef void (*OnDisconnectCb)(int client_id);

//************ serverNet API *************

/**
 * @brief Initialize the server: create TCP socket, bind, and listen
 *
 * @param _port    : port number to listen on
 * @param _on_msg  : callback fired when a complete TLV message arrives from a client
 * @param _on_disc : callback fired when a client disconnects
 * @return 0 on success, -1 on failure
 */
int ServerNet_Create(int port, OnMessageCb on_msg, OnDisconnectCb on_disc);

/**
 * @brief Enter the main select() event loop — blocks until StopRun is called
 */
void ServerNet_Run(void);

/**
 * @brief Signal the event loop to stop on the next iteration
 */
void ServerNet_StopRun(void);

/**
 * @brief Send a message to a specific connected client
 *
 * @param _client_id : the ID of the target client
 * @param _msg       : buffer containing the TLV bytes to send
 * @param _msg_len   : number of bytes to send
 * @return 0 on success, -1 if client not found or send failed
 */
int ServerNet_SendMsg(int client_id, const uint8_t *msg, int msg_len);

/**
 * @brief Close all client connections, free all resources
 */
void ServerNet_Destroy(void);

#endif
