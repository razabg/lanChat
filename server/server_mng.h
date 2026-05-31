#ifndef SERVER_MNG_H
#define SERVER_MNG_H

//************ ServerMng API *************

/**
 * @brief Initialize all sub-modules (ServerNet, UserMng, GroupMng) and start listening
 *
 * @param port : TCP port to listen on
 * @return 0 on success, -1 on failure
 */
int ServerMng_Create(int port);

/**
 * @brief Enter the main event loop — blocks until shutdown
 */
void ServerMng_Run(void);

/**
 * @brief Shut down all sub-modules and free all resources
 */
void ServerMng_Destroy(void);

#endif /* SERVER_MNG_H */
