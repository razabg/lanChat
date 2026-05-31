# Session Log — Client Side (No Standalone Apps)

**Project:** LAN Chat (Embedded & RTOS)
**Date:** 2026-05-30
**Scope:** `client_groups_mng` implementation + wiring into `client_mng`. Everything except `mc_sender.c` and `mc_receiver.c` (Phase 4).

> This document continues from `session_client.md` which covered `client_net`, `ui`, and `client_mng`.

---

## State at Start of This Session

| File | Status |
|------|--------|
| `common/protocol.c/.h` | Complete |
| `client/client_net.c/.h` | Complete |
| `client/ui.c/.h` | Complete |
| `client/client_mng.c/.h` | Complete — had 4 TODO markers for groups |
| `client/client_groups_mng.c/.h` | Empty |
| `client/client_main.c` | Complete |

---

## Part 1 — `client_groups_mng.h`

### Purpose

A local data structure on the client that tracks every group the user is currently in. The server does not know about this structure — it is purely client-side bookkeeping.

### Why HashMap over LinkedList

| Operation | LinkedList | HashMap |
|-----------|------------|---------|
| Add group | O(1) | O(1) |
| Find by name | O(n) | **O(1)** |
| Remove by name | O(n) | **O(1)** |
| Iterate all (logout) | O(n) | O(n) |

The critical operation is **find by group name** — done on every Leave and Logout. HashMap with the group name as key gives O(1) lookup. Same reasoning as the server side's `UserHash` and `GroupHash`.

`HashMap_ForEach` was confirmed available in `<ds/hashMap.h>`, which handles the logout iteration case.

### `GroupEntry` struct

```c
typedef struct {
    char     group_name[GROUP_NAME_SIZE];  /* lookup key, also stored here */
    char     mc_ip[MC_IP_SIZE];            /* "239.0.0.x" from server      */
    uint16_t mc_port;                      /* UDP multicast port            */
    pid_t    sender_pid;                   /* 0 until Phase 4 sets it      */
    pid_t    receiver_pid;                 /* 0 until Phase 4 sets it      */
} GroupEntry;
```

**PIDs start at 0** — on Linux `pid 0` means the process group, never a specific child process. This lets `Remove` and `RemoveAll` safely guard `if (pid > 0)` before calling `kill()`.

### Opaque handle

```c
typedef struct ClientGroupsMng ClientGroupsMng;
```

The struct definition lives only in the `.c` file. Callers can only hold a pointer — they cannot access the HashMap directly. Standard C encapsulation.

### Full API

| Function | Called by | When |
|----------|-----------|------|
| `ClientGroupsMng_Create` | `ClientMng_Create` | Once at startup |
| `ClientGroupsMng_Add` | `handle_create_group`, `handle_join_group` | On successful join/create |
| `ClientGroupsMng_Find` | Phase 4 window launcher | To write PIDs into the entry |
| `ClientGroupsMng_Remove` | `handle_leave_group` | On successful leave |
| `ClientGroupsMng_RemoveAll` | `handle_logout` | Kill all windows, reset map |
| `ClientGroupsMng_Destroy` | `ClientMng_Destroy` | Once at shutdown |

**Key design: `Remove` and `RemoveAll` kill PIDs internally.**
`GroupEntry` already has the PIDs. Encapsulating `kill()` inside the manager means `client_mng` just calls `Remove(group_name)` — no PID bookkeeping in the caller.

**`Find` returns a mutable `GroupEntry*`.**
Phase 4 uses this to write PIDs after launching terminals:
```c
GroupEntry *e = ClientGroupsMng_Find(mng->groups, group_name);
e->sender_pid   = sender_pid;
e->receiver_pid = receiver_pid;
```

---

## Part 2 — `client_groups_mng.c`

### Phase 1 — Struct + Static Helpers + Create + Destroy

#### The hidden struct

```c
struct ClientGroupsMng {
    HashMap *map;
};
```

#### `hash_string` — djb2

Maps a group name string to a `size_t` bucket index. Required by `HashMap_Create` as the `HashFunction`.

```c
static size_t hash_string(void *key)
{
    const char *str = (const char *)key;
    size_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + (size_t)c;  /* hash * 33 + c */
    return hash;
}
```

Magic seed `5381` + `hash * 33 + c` (written as bit-shift to avoid a multiply) produces well-distributed output for short ASCII strings.

#### `keys_equal` — strcmp wrapper

```c
static int keys_equal(void *a, void *b)
{
    return strcmp((const char *)a, (const char *)b) == 0;
}
```

Without this, HashMap uses **pointer equality** — two different `char*` pointers with the same content would be treated as different keys.

#### `kill_pids_action` — ForEach callback

```c
static int kill_pids_action(const void *key, void *value, void *context)
{
    (void)key; (void)context;
    GroupEntry *entry = (GroupEntry *)value;
    if (entry->sender_pid > 0)   kill(entry->sender_pid,   SIGTERM);
    if (entry->receiver_pid > 0) kill(entry->receiver_pid, SIGTERM);
    return 1;  /* must return non-zero to continue iteration */
}
```

**Critical: `pid > 0` guard.**
`kill(0, SIGTERM)` would signal the **entire process group**, not a specific process. The guard prevents this before Phase 4 sets real PIDs.

**Returns `1` not `0`.**
`HashMap_ForEach` stops early if the callback returns `0`. We always return `1` to visit every entry.

#### `ClientGroupsMng_Create`

```c
ClientGroupsMng *ClientGroupsMng_Create(void)
{
    ClientGroupsMng *mng = malloc(sizeof(ClientGroupsMng));
    if (!mng) { perror("ClientGroupsMng_Create: malloc"); return NULL; }

    mng->map = HashMap_Create(INITIAL_CAPACITY, hash_string, keys_equal);
    if (!mng->map) { free(mng); return NULL; }

    return mng;
}
```

Initial capacity = 16. HashMap rounds up to nearest prime internally.

#### `ClientGroupsMng_Destroy`

```c
void ClientGroupsMng_Destroy(ClientGroupsMng *mng)
{
    if (!mng) return;
    HashMap_ForEach(mng->map, kill_pids_action, NULL);  /* 1. kill PIDs    */
    HashMap_Destroy(&mng->map, free, free);              /* 2. free entries */
    free(mng);                                           /* 3. free struct  */
}
```

**Order matters:** PIDs must be killed *before* entries are freed. Reading freed memory to get a PID would be undefined behaviour.

---

### Phase 2 — Add + Find

#### `ClientGroupsMng_Add`

Two heap allocations per group:

```c
int ClientGroupsMng_Add(ClientGroupsMng *mng, const char *group_name,
                        const char *mc_ip, uint16_t mc_port)
{
    char *key = strdup(group_name);          /* HashMap owns this */
    if (!key) return -1;

    GroupEntry *entry = malloc(sizeof(GroupEntry));
    if (!entry) { free(key); return -1; }   /* no leak on partial failure */

    strncpy(entry->group_name, group_name, GROUP_NAME_SIZE - 1);
    entry->group_name[GROUP_NAME_SIZE - 1] = '\0';
    strncpy(entry->mc_ip, mc_ip, MC_IP_SIZE - 1);
    entry->mc_ip[MC_IP_SIZE - 1] = '\0';

    entry->mc_port      = mc_port;
    entry->sender_pid   = 0;
    entry->receiver_pid = 0;

    if (HashMap_Insert(mng->map, key, entry) != MAP_SUCCESS) {
        free(key); free(entry); return -1;
    }
    return 0;
}
```

**Why `strdup` the key?**
The `group_name` parameter is a stack buffer in the caller (`handle_create_group`). Once that function returns the buffer is gone. The HashMap must own a heap copy of the key that outlives the caller.

If `HashMap_Insert` fails, both `key` and `entry` are freed immediately — no leak.

#### `ClientGroupsMng_Find`

```c
GroupEntry *ClientGroupsMng_Find(ClientGroupsMng *mng, const char *group_name)
{
    void *value = NULL;
    if (HashMap_Find(mng->map, (void *)group_name, &value) != MAP_SUCCESS)
        return NULL;
    return (GroupEntry *)value;
}
```

Returns a **live pointer** into the map. The caller must never `free` it — the map owns the entry.

The cast of `group_name` to `void*` is safe because `HashMap_Find` uses `keys_equal` (strcmp) for comparison, not pointer equality. Passing a non-owned string as the search key works correctly.

---

### Phase 3 — Remove + RemoveAll

#### `ClientGroupsMng_Remove`

```c
int ClientGroupsMng_Remove(ClientGroupsMng *mng, const char *group_name)
{
    void *out_key   = NULL;
    void *out_value = NULL;

    if (HashMap_Remove(mng->map, (void *)group_name, &out_key, &out_value)
            != MAP_SUCCESS)
        return -1;

    GroupEntry *entry = (GroupEntry *)out_value;

    /* Kill before freeing — order is critical */
    if (entry->sender_pid > 0)   kill(entry->sender_pid,   SIGTERM);
    if (entry->receiver_pid > 0) kill(entry->receiver_pid, SIGTERM);

    free(out_key);  /* strdup'd key     */
    free(entry);    /* GroupEntry struct */
    return 0;
}
```

**Why `HashMap_Remove` gives back both pointers:**
`out_key` is the heap-allocated `strdup` copy stored in the map — that is the one that must be `free`'d, not the search key passed in. Without this, the key would leak every time a group is removed.

**Kill before free:** same reason as in Destroy — never read freed memory.

#### `ClientGroupsMng_RemoveAll`

```c
void ClientGroupsMng_RemoveAll(ClientGroupsMng *mng)
{
    HashMap_ForEach(mng->map, kill_pids_action, NULL);  /* 1. kill all PIDs    */
    HashMap_Destroy(&mng->map, free, free);              /* 2. free all entries */
    mng->map = HashMap_Create(INITIAL_CAPACITY,          /* 3. fresh empty map  */
                              hash_string, keys_equal);
}
```

**Destroy + recreate vs. remove one by one:**
Destroying and recreating is simpler and avoids modifying the map while iterating. The manager struct stays alive — only its contents are cleared. After `RemoveAll` the manager is immediately ready for the next login session.

---

## Part 3 — Wiring into `client_mng`

### Changes to `client_mng.h`

Added `groups` field and the include:

```c
#include "client_groups_mng.h"

typedef struct {
    ClientNet       *net;
    ClientGroupsMng *groups;   /* local group tracking */
    ClientState      state;
    int              running;
} ClientMng;
```

### Changes to `client_mng.c`

#### `ClientMng_Create` — fail-safe chain

```c
mng->net = ClientNet_Create(server_ip, port);
if (!mng->net) { free(mng); return NULL; }

mng->groups = ClientGroupsMng_Create();
if (!mng->groups) {
    ClientNet_Destroy(mng->net);   /* clean up net before returning */
    free(mng);
    return NULL;
}
```

If `groups` fails, `net` is destroyed first — no leak.

#### `ClientMng_Destroy` — destroy order

```c
void ClientMng_Destroy(ClientMng *mng)
{
    if (!mng) return;
    ClientGroupsMng_Destroy(mng->groups);  /* kills processes first */
    ClientNet_Destroy(mng->net);           /* then closes socket    */
    free(mng);
}
```

Groups are destroyed before the socket — running child processes may still use the network, so kill them before closing the connection.

#### Four TODOs filled

| Handler | Before | After |
|---------|--------|-------|
| `handle_create_group` | `/* TODO */` | `ClientGroupsMng_Add(mng->groups, group_name, mc_ip, mc_port)` |
| `handle_join_group` | `/* TODO */` | `ClientGroupsMng_Add(mng->groups, group_name, mc_ip, mc_port)` |
| `handle_leave_group` | `/* TODO */` | `ClientGroupsMng_Remove(mng->groups, group_name)` on success |
| `handle_logout` | `/* TODO */` | `ClientGroupsMng_RemoveAll(mng->groups)` then `state = SCREEN_1` |

Phase 4 TODOs remain in create/join handlers — exactly where window launching will plug in:
```c
/* TODO (Phase 4): launch mc_sender + mc_receiver via gnome-terminal,
 *   then set PIDs:
 *   GroupEntry *e = ClientGroupsMng_Find(mng->groups, group_name);
 *   e->sender_pid   = sender_pid;
 *   e->receiver_pid = receiver_pid;                                   */
```

---

## Compile Check — All Client Files

```bash
gcc -Wall -Wextra -g -I. -Iclient -Icommon -I~/projectsLinux/DS/HashMap \
    -c client/client_mng.c         # OK
    -c client/client_groups_mng.c  # OK
    -c client/client_net.c         # OK
    -c client/client_main.c        # OK
    -c client/ui.c                 # OK
    -c common/protocol.c           # OK
```

Zero warnings across all six files.

---

## What Remains (Phase 4)

| File | What it does |
|------|-------------|
| `mc_sender.c` | Standalone app: reads stdin, sends to UDP multicast address, reports PID via message queue |
| `mc_receiver.c` | Standalone app: joins multicast group, prints received messages, reports PID via message queue |
| Window launching | `system("gnome-terminal -- ./mc_sender ...")` inside `handle_create_group` / `handle_join_group` |
| PID retrieval | Read PID from POSIX/SysV message queue after launch, store via `ClientGroupsMng_Find` |

The hooks for Phase 4 are already in place — the TODO comments in `handle_create_group` and `handle_join_group` show exactly where to insert the code.

---

## Full Client Module Dependency Map

```
client_main
    └── ClientMng_Create / Run / Destroy
            ├── client_net      (TCP socket — send/recv bytes)
            ├── client_groups_mng  (HashMap — local group tracking)
            │       └── ds/hashMap.h  (libds.so)
            ├── ui              (terminal menus + input)
            └── protocol        (tlv_encode / tlv_decode / tlv_find_field)
```