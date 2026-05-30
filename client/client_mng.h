#ifndef CLIENT_MNG_H
#define CLIENT_MNG_H

#include "client_net.h"
#include "client_groups_mng.h"

/* ── Screen state ───────────────────────────────────────────────────────────
 * Tracks which screen the user is currently on.
 * SCREEN_1 = pre-login  (Register / Login / Exit)
 * SCREEN_2 = post-login (Create Group / Join Group / Leave Group / Logout)
 * The Run loop checks this every iteration to decide which menu to show.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef enum
{
    SCREEN_1 = 0,
    SCREEN_2 = 1
} ClientState;

/* ── ClientMng struct ───────────────────────────────────────────────────────
 * The central object of the client.  Holds everything ClientMng needs
 * to operate across all function calls:
 *
 *   net     — pointer to the ClientNet (TCP connection to the server).
 *             All sends and receives go through this.
 *
 *   state   — which screen we are on (SCREEN_1 or SCREEN_2).
 *             Changed by handle_login (1→2) and handle_logout (2→1).
 *
 *   running — loop-control flag.  Set to 0 by MENU_EXIT or a fatal error
 *             to break out of ClientMng_Run's while loop.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    ClientNet       *net;
    ClientGroupsMng *groups;   /* local group tracking — Add/Remove/RemoveAll */
    ClientState      state;
    int              running;
} ClientMng;

/* ── ClientMng_Create ───────────────────────────────────────────────────────
 * Allocates and initialises a ClientMng.
 *
 * What it does:
 *   1. malloc the ClientMng struct.
 *   2. Calls ClientNet_Create(server_ip, port) to open the TCP socket
 *      and connect to the server.
 *   3. Sets state = SCREEN_1 and running = 1.
 *
 * Parameters:
 *   server_ip — dotted-decimal IPv4 string of the server, e.g. "192.168.1.5"
 *   port      — TCP port the server is listening on
 *
 * Returns a heap-allocated ClientMng on success, NULL on failure
 * (e.g. server unreachable, bad IP, malloc failed).
 * ─────────────────────────────────────────────────────────────────────────── */
ClientMng *ClientMng_Create(const char *server_ip, uint16_t port);

/* ── ClientMng_Run ──────────────────────────────────────────────────────────
 * Enters the main interaction loop.  Blocks until the user exits.
 *
 * What it does:
 *   Loops while mng->running == 1.
 *   Each iteration:
 *     - If state == SCREEN_1: shows the main menu, reads the choice,
 *       dispatches to handle_register or handle_login.
 *       MENU_EXIT sets running = 0.
 *     - If state == SCREEN_2: shows the group menu, reads the choice,
 *       dispatches to handle_create_group / handle_join_group /
 *       handle_leave_group / handle_logout.
 *     - Invalid input calls showMessage("Invalid option, try again.").
 *
 * This function returns only when the loop exits (user chose Exit,
 * or a fatal network error occurred).
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientMng_Run(ClientMng *mng);

/* ── ClientMng_Destroy ──────────────────────────────────────────────────────
 * Releases all resources owned by ClientMng.
 *
 * What it does:
 *   1. Calls ClientNet_Destroy(mng->net) — closes the TCP socket.
 *   2. Frees the ClientMng struct itself.
 *
 * Safe to call with NULL (no-op).
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientMng_Destroy(ClientMng *mng);

#endif /* CLIENT_MNG_H */