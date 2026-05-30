#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>      /* kill(), SIGTERM */
#include <ds/hashMap.h>
#include "client_groups_mng.h"

#define INITIAL_CAPACITY 16

/* ── Struct definition (opaque from outside) ─────────────────────────────────
 * Hidden here so callers can only hold a ClientGroupsMng* and cannot
 * access the HashMap directly.
 * ─────────────────────────────────────────────────────────────────────────── */
struct ClientGroupsMng
{
    HashMap *map;
};

/* ─── Phase 1: Static helpers + Create + Destroy ────────────────────────────*/

/* ── hash_string ──────────────────────────────────────────────────────────────
 * djb2 hash — maps a group name string to a size_t bucket index.
 * Required by HashMap_Create as the HashFunction.
 *
 * djb2 starts with the magic seed 5381 and for each character computes:
 *   hash = hash * 33 + c
 * written as (hash << 5) + hash + c to avoid a multiply instruction.
 * This produces a well-distributed hash for short ASCII strings.
 * ─────────────────────────────────────────────────────────────────────────── */
static size_t hash_string(void *key)
{
    const char *str = (const char *)key;
    size_t hash = 5381;
    int c;

    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + (size_t)c;

    return hash;
}

/* ── keys_equal ───────────────────────────────────────────────────────────────
 * EqualityFunction required by HashMap_Create.
 * Returns 1 (true) when both string keys are identical, 0 otherwise.
 * ─────────────────────────────────────────────────────────────────────────── */
static int keys_equal(void *a, void *b)
{
    return strcmp((const char *)a, (const char *)b) == 0;
}

/* ── kill_pids_action ─────────────────────────────────────────────────────────
 * KeyValueActionFunction passed to HashMap_ForEach.
 * Called once per entry — kills sender and receiver PIDs with SIGTERM
 * if they are set (> 0).  PIDs are 0 before Phase 4 sets them, so the
 * guard prevents calling kill(0, SIGTERM) which would signal the process group.
 *
 * Must return non-zero to continue iteration (return 0 stops ForEach early).
 * ─────────────────────────────────────────────────────────────────────────── */
static int kill_pids_action(const void *key, void *value, void *context)
{
    (void)key;
    (void)context;

    GroupEntry *entry = (GroupEntry *)value;

    if (entry->sender_pid > 0)   kill(entry->sender_pid,   SIGTERM);
    if (entry->receiver_pid > 0) kill(entry->receiver_pid, SIGTERM);

    return 1; /* continue iteration */
}

/* ── ClientGroupsMng_Create ───────────────────────────────────────────────── */
ClientGroupsMng *ClientGroupsMng_Create(void)
{
    ClientGroupsMng *mng = malloc(sizeof(ClientGroupsMng));
    if (!mng)
    {
        perror("ClientGroupsMng_Create: malloc");
        return NULL;
    }

    mng->map = HashMap_Create(INITIAL_CAPACITY, hash_string, keys_equal);
    if (!mng->map)
    {
        free(mng);
        return NULL;
    }

    return mng;
}

/* ── ClientGroupsMng_Destroy ──────────────────────────────────────────────────
 * Called once at app shutdown (from ClientMng_Destroy).
 *
 * Order matters:
 *   1. ForEach  — kill all PIDs before the entries are freed.
 *   2. Destroy  — HashMap_Destroy frees every strdup'd key (free) and
 *                 every GroupEntry (free).  Passing free as both destructors
 *                 handles this in one call.
 *   3. free(mng)
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientGroupsMng_Destroy(ClientGroupsMng *mng)
{
    if (!mng) return;

    HashMap_ForEach(mng->map, kill_pids_action, NULL);
    HashMap_Destroy(&mng->map, free, free);
    free(mng);
}

/* ─── Phase 2: Add + Find ───────────────────────────────────────────────────*/

/* ── ClientGroupsMng_Add ──────────────────────────────────────────────────────
 *
 * Two heap allocations per group:
 *   key   = strdup(group_name) — the HashMap owns this string.
 *   entry = malloc(GroupEntry) — the HashMap owns this struct.
 *
 * Both are freed by HashMap_Destroy / HashMap_Remove when the entry is removed.
 *
 * PIDs are set to 0 here.  Phase 4 will call ClientGroupsMng_Find and write
 * the real PIDs into the returned entry pointer after launching terminals.
 * ─────────────────────────────────────────────────────────────────────────── */
int ClientGroupsMng_Add(ClientGroupsMng *mng, const char *group_name,
                        const char *mc_ip, uint16_t mc_port)
{
    char *key = strdup(group_name);
    if (!key) return -1;

    GroupEntry *entry = malloc(sizeof(GroupEntry));
    if (!entry) { free(key); return -1; }

    /* Safe string copies — guard against buffer overrun */
    strncpy(entry->group_name, group_name, GROUP_NAME_SIZE - 1);
    entry->group_name[GROUP_NAME_SIZE - 1] = '\0';

    strncpy(entry->mc_ip, mc_ip, MC_IP_SIZE - 1);
    entry->mc_ip[MC_IP_SIZE - 1] = '\0';

    entry->mc_port      = mc_port;
    entry->sender_pid   = 0;
    entry->receiver_pid = 0;

    if (HashMap_Insert(mng->map, key, entry) != MAP_SUCCESS)
    {
        free(key);
        free(entry);
        return -1;
    }

    return 0;
}

/* ── ClientGroupsMng_Find ─────────────────────────────────────────────────────
 *
 * Returns a pointer directly into the live GroupEntry stored in the map.
 * The caller must NOT free this pointer — the map owns the entry.
 *
 * Phase 4 uses this to set PIDs after launching terminal windows:
 *   GroupEntry *e = ClientGroupsMng_Find(mng, group_name);
 *   e->sender_pid   = sender_pid;
 *   e->receiver_pid = receiver_pid;
 * ─────────────────────────────────────────────────────────────────────────── */
GroupEntry *ClientGroupsMng_Find(ClientGroupsMng *mng, const char *group_name)
{
    void *value = NULL;

    if (HashMap_Find(mng->map, (void *)group_name, &value) != MAP_SUCCESS)
        return NULL;

    return (GroupEntry *)value;
}

/* ─── Phase 3: Remove + RemoveAll ───────────────────────────────────────────*/

/* ── ClientGroupsMng_Remove ───────────────────────────────────────────────────
 *
 * HashMap_Remove gives back BOTH the key pointer and value pointer.
 * This is crucial — we need them to:
 *   1. Kill the PIDs stored in the entry (before freeing).
 *   2. free(out_key)   — the strdup'd group name string.
 *   3. free(out_value) — the GroupEntry struct.
 *
 * The cast of group_name to (void*) is safe — HashMap_Find uses keys_equal
 * (strcmp) for comparison, not pointer equality, so passing a non-owned
 * string as the search key works correctly.
 * ─────────────────────────────────────────────────────────────────────────── */
int ClientGroupsMng_Remove(ClientGroupsMng *mng, const char *group_name)
{
    void *out_key   = NULL;
    void *out_value = NULL;

    if (HashMap_Remove(mng->map, (void *)group_name, &out_key, &out_value)
            != MAP_SUCCESS)
        return -1;

    GroupEntry *entry = (GroupEntry *)out_value;

    /* Kill processes before freeing the entry that holds their PIDs */
    if (entry->sender_pid > 0)   kill(entry->sender_pid,   SIGTERM);
    if (entry->receiver_pid > 0) kill(entry->receiver_pid, SIGTERM);

    free(out_key);   /* strdup'd key */
    free(entry);     /* GroupEntry   */

    return 0;
}

/* ── ClientGroupsMng_RemoveAll ────────────────────────────────────────────────
 *
 * Called on logout — must close all chat windows and reset the list so
 * the user can log in again and join new groups in the same session.
 *
 * Order matters:
 *   1. ForEach  — kill all PIDs while entries still exist.
 *   2. Destroy  — free every key and entry via the free destructor.
 *   3. Create   — fresh empty map, ready for the next login session.
 *
 * We do NOT free(mng) here — the manager itself stays alive.
 * Only its contents are cleared.
 * ─────────────────────────────────────────────────────────────────────────── */
void ClientGroupsMng_RemoveAll(ClientGroupsMng *mng)
{
    HashMap_ForEach(mng->map, kill_pids_action, NULL);
    HashMap_Destroy(&mng->map, free, free);
    mng->map = HashMap_Create(INITIAL_CAPACITY, hash_string, keys_equal);
}