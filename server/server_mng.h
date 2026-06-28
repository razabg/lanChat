#ifndef SERVER_MNG_H
#define SERVER_MNG_H

/* ─── Opaque manager type ───────────────────────────────────────────────── */
typedef struct ServerMng ServerMng;

//************ ServerMng API *************

/**
 * @brief Initialize all sub-modules (ServerNet, UserMng, GroupMng) and start listening
 *
 * @param port : TCP port to listen on
 * @return ServerMng* on success, NULL on failure
 */
ServerMng *ServerMng_Create(int port);

/**
 * @brief Enter the main event loop — blocks until shutdown
 *
 * @param mng : the server manager
 */
void ServerMng_Run(ServerMng *mng);

/**
 * @brief Shut down all sub-modules and free all resources
 *
 * @param mng : pointer to the manager pointer (set to NULL on destroy)
 */
void ServerMng_Destroy(ServerMng **mng);

#endif /* SERVER_MNG_H */
