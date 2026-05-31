# LanChat — Full Bug Report

**Date:** 2026-05-31  
**Scope:** All source files — server side, client side, common protocol  
**Files reviewed:**
- `server/server_net.c`
- `server/server_mng.c`
- `server/user_mng.c`
- `server/group_mng.c`
- `client/client_mng.c`
- `client/client_net.c`
- `client/client_groups_mng.c`
- `client/mc_sender.c`
- `client/mc_receiver.c`
- `common/protocol.c`

---

## Summary Table

| # | File | Severity | Description |
|---|------|----------|-------------|
| 1 | `group_mng.c` | **Critical** | `HashMap_Remove` called inside `HashMap_ForEach` — iterator corruption |
| 2 | `user_mng.c` | **Critical** | Stack buffer stored as HashMap key — dangling pointer |
| 3 | `server_net.c` | **Moderate** | `send()` not retried on partial write in `ServerNet_SendMsg` |
| 4 | `server_net.c` | **Moderate** | `select()` EINTR treated as fatal error — kills the server event loop |
| 5 | `server_net.c` | **Moderate** | `malloc` result not checked in `accept_new_client` |
| 6 | `group_mng.c` | **Moderate** | No duplicate-member check in `GroupMng_Join` |
| 7 | `group_mng.c` | **Minor** | `GroupMng_Leave` always returns `STATUS_SUCCESS` |
| 8 | `group_mng.c` | **Minor** | `calloc`/`malloc` results not checked in `GroupMng_CreateGroup` |
| 9 | `client_mng.c` | **Minor** | `username` field uninitialized after `malloc` in `ClientMng_Create` |
| 10 | `client_mng.c` | **Minor** | `mq_receive` blocks forever if terminal launch fails |
| 11 | `server_net.c` | **Minor** | `recv()` EINTR treated as client disconnect |

---

## Detailed Bug Descriptions

---

### BUG 1 — `group_mng.c` — `HashMap_Remove` called inside `HashMap_ForEach` *(Critical)*

**Function:** `remove_client_from_group()`, called from `remove_client_from_group_cb()`,
called by `HashMap_ForEach` in `GroupMng_RemoveClientFromAll()`

**Trigger:** A client disconnects (or logs out) while being the **last member** of at least one group.

**What happens step by step:**
1. `on_disconnect(client_id)` fires → calls `GroupMng_RemoveClientFromAll(client_id)`.
2. `GroupMng_RemoveClientFromAll` calls `HashMap_ForEach(s_group_hash, remove_client_from_group_cb, &client_id)`.
3. `HashMap_ForEach` begins iterating over every group in the hash map.
4. For a group where this client is the **last member**, `remove_client_from_group` detects `member_count == 0` and calls:
   ```c
   HashMap_Remove(s_group_hash, group->group_name, &key, &removed);
   ```
5. This removes an entry from `s_group_hash` — the **same map that ForEach is currently iterating**.
6. ForEach's internal iterator is now pointing at either freed memory or a skipped/corrupted bucket.
7. The next ForEach step is **undefined behavior** — likely a crash (segfault) or silent data corruption.

**Affected code:**
```c
/* group_mng.c — remove_client_from_group() */
if (group->member_count <= 0) {
    char *ip_copy = strdup(group->mc_ip);
    void *key, *removed;
    HashMap_Remove(s_group_hash, group->group_name, &key, &removed);  /* DANGER: modifies map mid-iteration */
    destroy_group(group);
    QueueInsert(s_free_mc_ip_queue, ip_copy);
}
```

**Fix:**
Do not call `HashMap_Remove` inside the ForEach callback. Instead:
1. During ForEach — only remove the client from the member list and decrement the counter.
2. After ForEach completes — do a second pass over the map to find and destroy any groups with `member_count == 0`.

---

### BUG 2 — `user_mng.c` — Stack-allocated buffer stored as HashMap key *(Critical)*

**Function:** `UserMng_Login()`, lines 102–104

**What happens step by step:**
1. `UserMng_Login` is called. It declares a local (stack) array:
   ```c
   char id_key[16];
   snprintf(id_key, sizeof(id_key), "%d", client_id);
   ```
2. This local array is passed directly to `HashMap_Insert`:
   ```c
   HashMap_Insert(s_user_hash_by_client_id, id_key, user);
   ```
3. The HashMap stores the **pointer** to `id_key` as the key — it does not copy the string.
4. `UserMng_Login` returns. Its stack frame is destroyed. `id_key` no longer exists.
5. The HashMap is now holding a **dangling pointer** — a pointer to memory that was on the stack and is now invalid.
6. Later, when `UserMng_GetByClientId` calls `HashMap_Find`, the map compares search keys against this dangling pointer using `strcmp`. Accessing the dangling pointer is **undefined behavior** — it may return garbage results or crash.

**Affected code:**
```c
/* user_mng.c — UserMng_Login() */
char id_key[16];                                           /* stack-allocated */
snprintf(id_key, sizeof(id_key), "%d", client_id);
HashMap_Insert(s_user_hash_by_client_id, id_key, user);  /* stores the stack pointer — BUG */
/* function returns here; id_key is gone; HashMap has a dangling key */
```

**Why it may seem to work in testing:**
On most systems, the old stack memory is not immediately overwritten if no other function runs. In light testing, `strcmp` against the dangling pointer might return the correct result by accident. But this is not reliable and will eventually fail.

**Fix:**
```c
char *key = strdup(id_key);   /* heap-allocated copy — persists after function returns */
HashMap_Insert(s_user_hash_by_client_id, key, user);
```
And in `UserMng_LogoutByClientId`, free the key returned by `HashMap_Remove`:
```c
void *key, *val;
HashMap_Remove(s_user_hash_by_client_id, id_key, &key, &val);
free(key);  /* must free the strdup'd key */
```

---

### BUG 3 — `server_net.c` — Single `send()` call without retry loop *(Moderate)*

**Function:** `ServerNet_SendMsg()`, lines 87–88

**What happens:**
TCP is a stream protocol. A single `send()` call is **not guaranteed** to transmit all the bytes you give it. The kernel can accept only part of the data if its send buffer is temporarily full, and returns the number of bytes actually sent — which can be less than `msg_len`. This is called a **partial write** and is not an error.

The current code treats a partial write as an error and returns `-1`:
```c
int sent = send(node->socket_fd, msg, msg_len, 0);
return (sent == msg_len) ? 0 : -1;   /* partial write silently treated as failure */
```

The result:
- A TLV message is partially sent — the client receives a truncated, unparseable stream.
- The protocol desyncs. All subsequent messages are misinterpreted.
- The server returns `-1` to the caller but the client is not disconnected, leaving both sides in an inconsistent state.

**Comparison:** The client's `ClientNet_SendMsg` correctly uses a retry loop — the server must do the same.

**Fix:**
```c
int ServerNet_SendMsg(int client_id, const uint8_t *msg, int msg_len)
{
    /* ... find node ... */
    int sent = 0;
    while (sent < msg_len) {
        ssize_t n = send(node->socket_fd, msg + sent, msg_len - sent, 0);
        if (n <= 0) { perror("ServerNet_SendMsg: send"); return -1; }
        sent += (int)n;
    }
    return 0;
}
```

---

### BUG 4 — `server_net.c` — `select()` EINTR treated as fatal error *(Moderate)*

**Function:** `ServerNet_Run()`, lines 109–112

**What happens:**
When any signal is delivered to the server process while it is blocked inside `select()` — for example `SIGCHLD` (a child process exited), `SIGHUP`, or `SIGPIPE` — the OS **interrupts** `select()` and returns `-1` with `errno == EINTR`. This is not an error. It simply means a signal arrived and `select()` must be called again.

The current code breaks out of the event loop on any `select` error, including EINTR:
```c
if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
    perror("select");
    break;   /* entire server event loop exits on a harmless signal */
}
```

**Result:** A single signal shuts down the server — no clients can connect anymore, no messages are processed. On a system where `SIGCHLD` fires regularly the server could die almost immediately after startup.

**Fix:**
```c
if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
    if (errno == EINTR) continue;   /* signal interrupted — just retry select */
    perror("select");
    break;
}
```

---

### BUG 5 — `server_net.c` — `malloc` result not checked in `accept_new_client` *(Moderate)*

**Function:** `accept_new_client()`, line 171

**What happens:**
`malloc` can return `NULL` if the system is out of memory. The return value is not checked, and the very next line dereferences it:
```c
ClientNode *node = malloc(sizeof(ClientNode));
node->socket_fd = client_fd;   /* crash if malloc returned NULL */
```

If this happens:
- The server crashes with a segfault.
- The newly accepted client socket (`client_fd`) is never closed — it leaks a file descriptor.
- All existing connected clients lose their sessions.

**Fix:**
```c
ClientNode *node = malloc(sizeof(ClientNode));
if (!node) {
    perror("accept_new_client: malloc");
    close(client_fd);   /* don't leak the socket */
    return;
}
```

---

### BUG 6 — `group_mng.c` — No duplicate-member check in `GroupMng_Join` *(Moderate)*

**Function:** `GroupMng_Join()`, lines 94–112

**What happens:**
Before adding a client to a group, the code never checks whether `client_id` is already in the member list. If a client sends `JOIN_GROUP_REQ` for a group they already belong to:
1. `member_count` is incremented a second time (now 2 instead of 1).
2. The `client_id` is inserted a second time into the members linked list.
3. When the client leaves once, the count drops to 1 — the group is never auto-closed.
4. The multicast IP is never returned to the pool — a **resource leak**.
5. On disconnect, the client is removed once from the member list (count drops to 0) but the second duplicate entry still exists — the group is destroyed while it still has a member in its list.

**Fix:**
At the start of `GroupMng_Join`, walk `group->members` and check if `client_id` is already present. If yes, return `STATUS_ALREADY_IN_GROUP`.

---

### BUG 7 — `group_mng.c` — `GroupMng_Leave` always returns `STATUS_SUCCESS` *(Minor)*

**Function:** `GroupMng_Leave()`, lines 114–122

**What happens:**
`remove_client_from_group` returns `1` whether or not the client was actually found and removed. The return value is ignored and `GroupMng_Leave` unconditionally returns `STATUS_SUCCESS`:
```c
remove_client_from_group((Group *)val, client_id);  /* return value ignored */
return STATUS_SUCCESS;                               /* always success, even if not in group */
```

A client that was never in the group (or already left) receives a `SUCCESS` response instead of `STATUS_NOT_IN_GROUP`.

**Fix:**
Make `remove_client_from_group` return a distinct value when the client ID is not found, check that return value in `GroupMng_Leave`, and return `STATUS_NOT_IN_GROUP` accordingly.

---

### BUG 8 — `group_mng.c` — `calloc`/`malloc` not checked in `GroupMng_CreateGroup` *(Minor)*

**Function:** `GroupMng_CreateGroup()`, lines 74 and 82

**What happens:**
Two heap allocations are not null-checked:
```c
Group *group = calloc(1, sizeof(Group));
strncpy(group->group_name, ...);   /* crash if calloc returned NULL */

int *id = malloc(sizeof(int));
*id = client_id;                   /* crash if malloc returned NULL */
```

If `calloc` fails:
- The server crashes (NULL dereference).
- The multicast IP that was already dequeued is leaked.

If the second `malloc` fails:
- The group record is created and inserted into the hash with `member_count = 1` but an empty members list — inconsistent state that will cause incorrect behavior on leave/disconnect.

**Fix:** Check both allocations. On failure, return `STATUS_SERVER_ERROR` and return the dequeued IP back to the queue.

---

### BUG 9 — `client_mng.c` — `username` field uninitialized after `malloc` *(Minor)*

**Function:** `ClientMng_Create()`, line 194

**What happens:**
`malloc` does **not** zero-initialize memory. After `ClientMng_Create` returns, `mng->username` contains whatever bytes happened to be in that heap region — garbage data.

```c
ClientMng *mng = malloc(sizeof(ClientMng));
mng->state   = SCREEN_1;
mng->running = 1;
/* mng->username is never initialized — contains garbage */
```

In normal flow this is safe because `username` is only read by `launch_chat_windows`, which is only called after a successful login. But it is a latent bug — if the control flow ever reaches `launch_chat_windows` without a prior login, garbage is passed to `mc_sender` as the username argument.

**Fix:** Use `calloc(1, sizeof(ClientMng))` instead of `malloc`, which zeroes all fields automatically.

---

### BUG 10 — `client_mng.c` — `mq_receive` blocks forever if terminal launch fails *(Minor)*

**Function:** `launch_chat_windows()`, lines 153–175

**What happens:**
After `system("gnome-terminal -- ./mc_receiver ...")` launches the terminal, the code immediately calls:
```c
mq_receive(mq_r, (char *)&receiver_pid, sizeof(pid_t), NULL);
```

This call blocks **indefinitely** until a message arrives on the queue. If:
- `gnome-terminal` is not installed on this machine,
- `./mc_receiver` binary does not exist in the current working directory,
- The terminal window fails to open for any reason,

...then `mc_receiver` never starts, never sends its PID, and `mq_receive` hangs forever. The client application freezes. The user cannot type anything or exit — the only option is to kill the process from another terminal.

**Fix:** Use `mq_timedreceive` with a deadline a few seconds in the future. If it times out, print an error, clean up both queues, and return without storing PIDs.

---

### BUG 11 — `server_net.c` — `recv()` EINTR treated as client disconnect *(Minor)*

**Function:** `process_all_clients()`, lines 217–222

**What happens:**
Same root cause as Bug 4 (EINTR on blocking system calls). If a signal arrives while `recv()` is blocking, it returns `-1` with `errno == EINTR`. This is not a disconnect — it means a signal was delivered and `recv()` should be retried.

The current code checks `bytes <= 0` and treats this as a disconnect:
```c
if (bytes <= 0) {
    s_on_disconnect(node->client_id);  /* fires on EINTR, not just real disconnects */
    close(node->socket_fd);
    ListItrRemove(current);
    free(node);
    continue;
}
```

**Result:** A client that is still fully connected gets forcibly disconnected simply because a signal arrived at the wrong moment. All their group memberships and session state are wiped.

**Fix:**
```c
if (bytes < 0) {
    if (errno == EINTR) continue;  /* signal interrupted recv — retry next select cycle */
    /* real socket error — fall through to disconnect handling */
}
if (bytes <= 0) {
    s_on_disconnect(node->client_id);
    close(node->socket_fd);
    ListItrRemove(current);
    free(node);
    continue;
}
```

---

## Files with No Bugs Found

| File | Status |
|------|--------|
| `common/protocol.c` | Clean — TLV encode/decode/find logic is correct |
| `server/server_mng.c` | Clean — routing, handler logic, TLV parsing all correct |
| `client/client_net.c` | Clean — write loop, TLV reassembly buffer correct |
| `client/client_groups_mng.c` | Clean — HashMap usage, strdup keys, PID kill logic correct |
| `client/mc_sender.c` | Clean — signal handler, UDP sendto, PID queue correct |
| `client/mc_receiver.c` | Clean — `_GNU_SOURCE`, IP_ADD_MEMBERSHIP, EINTR exit path correct |

---

## Recommended Fix Priority

1. **Fix Bug 1 first** (ForEach + Remove) — crashes on any normal disconnect scenario.
2. **Fix Bug 2** (dangling HashMap key) — undefined behavior that corrupts all session lookups.
3. **Fix Bug 3** (partial send) — silently breaks the TLV protocol stream.
4. **Fix Bugs 4 and 11 together** — both are EINTR handling issues, same one-line fix pattern.
5. **Fix Bug 5** (malloc null check) — defensive but important for stability under load.
6. **Fix Bug 6** (duplicate join) — needed before integration testing with multiple clients.
7. **Fix remaining minor bugs** (7, 8, 9, 10) before final submission / valgrind check.