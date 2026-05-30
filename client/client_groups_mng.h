#ifndef CLIENT_GROUPS_MNG_H
#define CLIENT_GROUPS_MNG_H

#include <stdint.h>
#include <sys/types.h>   /* pid_t */
#include "ui.h"          /* GROUP_NAME_SIZE */

#define MC_IP_SIZE 64

/* ── GroupEntry ─────────────────────────────────────────────────────────────
 * One entry per group the client is currently a member of.
 *
 * group_name    — the group's name, used as the HashMap key for lookup.
 * mc_ip         — multicast IPv4 string received from the server ("239.0.0.x").
 * mc_port       — UDP multicast port received from the server.
 * sender_pid    — PID of the mc_sender terminal process.
 *                 0 until Phase 4 wires up window launching.
 * receiver_pid  — PID of the mc_receiver terminal process.
 *                 0 until Phase 4 wires up window launching.
 *
 * PIDs are stored here so ClientGroupsMng_Remove / RemoveAll can call
 * kill(pid, SIGTERM) on the correct processes without the caller needing
 * to know the details.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct
{
    char     group_name[GROUP_NAME_SIZE];
    char     mc_ip[MC_IP_SIZE];
    uint16_t mc_port;
    pid_t    sender_pid;
    pid_t    receiver_pid;
} GroupEntry;

/* Opaque handle — internals (the HashMap) are hidden in the .c file */
typedef struct ClientGroupsMng ClientGroupsMng;

/* ── ClientGroupsMng_Create ─────────────────────────────────────────────────
 * Allocates the manager and creates an empty HashMap.
 *
 * Returns a heap-allocated ClientGroupsMng on success, NULL on failure.
 * ─────────────────────────────────────────────────────────────────────────── */
ClientGroupsMng *ClientGroupsMng_Create(void);

/* ── ClientGroupsMng_Add ────────────────────────────────────────────────────
 * Inserts a new group entry into the map.
 *
 * Internally strdup's group_name as the key and malloc's a GroupEntry
 * as the value.  sender_pid and receiver_pid are initialised to 0 —
 * Phase 4 sets them via ClientGroupsMng_Find after launching terminals.
 *
 * Returns  0 on success.
 * Returns -1 if the group already exists or on allocation failure.
 * ─────────────────────────────────────────────────────────────────────────── */
int ClientGroupsMng_Add(ClientGroupsMng *mng, const char *group_name,
                        const char *mc_ip, uint16_t mc_port);

/* ── ClientGroupsMng_Find ───────────────────────────────────────────────────
 * Looks up a group by name.
 *
 * Returns a pointer to the live GroupEntry inside the map on success.
 * The caller may read or write any field (e.g. set sender_pid / receiver_pid
 * after launching terminal windows in Phase 4).
 *
 * Returns NULL if the group is not found.
 * ─────────────────────────────────────────────────────────────────────────── */
GroupEntry *ClientGroupsMng_Find(ClientGroupsMng *mng, const char *group_name);

/* ── ClientGroupsMng_Remove ─────────────────────────────────────────────────
 * Removes a single group entry by name.
 *
 * Before freeing the entry it kills sender_pid and receiver_pid with
 * SIGTERM (only if the PID is > 0), closing the chat terminal windows.
 * Then frees the strdup'd key and the GroupEntry.
 *
 * Returns  0 on success.
 * Returns -1 if the group is not found.
 * ─────────────────────────────────────────────────────────────────────────── */
int ClientGroupsMng_Remove(ClientGroupsMng *mng, const char *group_name);

/* ── ClientGroupsMng_RemoveAll ──────────────────────────────────────────────
 * Kills all stored PIDs and clears every entry from the map.
 *
 * Called on logout — closes every open chat window then resets the map
 * so it is ready for the next login session.
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientGroupsMng_RemoveAll(ClientGroupsMng *mng);

/* ── ClientGroupsMng_Destroy ────────────────────────────────────────────────
 * Kills all stored PIDs, destroys the HashMap, and frees the manager.
 *
 * Called once at application shutdown (from ClientMng_Destroy).
 * Safe to call with NULL.
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientGroupsMng_Destroy(ClientGroupsMng *mng);

#endif /* CLIENT_GROUPS_MNG_H */