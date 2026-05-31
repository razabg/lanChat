# LAN Chat — Bug Report

---

## Bug 1 — HashMap_Remove bug in libds.so

**File:** `~/projectsLinux/DS/HashMap/hashMap.c`

**Symptom:** Leaving a group succeeded on the server side but the client's local group entry was never removed. The terminal windows did not close on leave/logout.

**Root cause:** `HashMap_Remove` was casting the list iterator directly to a `Pair*` instead of calling `ListItrGet` first:
```c
// BUG
Pair* _pair = (Pair*)_begin;

// FIX
Pair* _pair = (Pair*)ListItrGet(_begin);
```
This caused `_pair->m_key` to read garbage memory, so the equality check always failed and the entry was never found for removal. `HashMap_Find` worked correctly because it used `ListItrGet` properly.

**Fix:** Added `ListItrGet` calls in `HashMap_Remove`, same as `HashMap_Insert` already did.

---

## Bug 2 — Stack-allocated HashMap key dangling pointer

**File:** `server/user_mng.c` — `UserMng_Login`

**Symptom:** After logout, logging in again returned `STATUS_ALREADY_LOGGED_IN`.

**Root cause:** The key for `s_user_hash_by_client_id` was a stack-allocated buffer:
```c
char id_key[16];  // stack — gone after function returns
snprintf(id_key, sizeof(id_key), "%d", client_id);
HashMap_Insert(s_user_hash_by_client_id, id_key, user);
```
The HashMap stored the pointer to `id_key`. After `UserMng_Login` returned, `id_key` was invalid. Later `HashMap_Find` in `UserMng_LogoutByClientId` compared against garbage memory, returned NULL, and `user->is_active` was never set to 0.

**Fix:** Heap-allocate the key so it persists:
```c
char *id_key = malloc(16);
snprintf(id_key, 16, "%d", client_id);
HashMap_Insert(s_user_hash_by_client_id, id_key, user);
// free(key) called in UserMng_LogoutByClientId after HashMap_Remove
```

---

## Bug 3 — Modifying HashMap during ForEach iteration (server crash)

**File:** `server/group_mng.c` — `GroupMng_RemoveClientFromAll`

**Symptom:** Server crashed with segmentation fault when a client logged out or disconnected while in a group.

**Root cause:** `GroupMng_RemoveClientFromAll` used `HashMap_ForEach` to iterate all groups. Inside the callback, when the last member left a group, `HashMap_Remove` was called on the same hash being iterated. This invalidated the iterator's internal node pointer, causing the next iteration to access freed memory.

**Fix:** Two-pass approach — collect empty groups in a list during ForEach, then destroy them after ForEach completes:
```c
// Pass 1 — ForEach (hash untouched)
//   find client in each group, decrement count
//   if empty → add to ctx.to_destroy list

// Pass 2 — after ForEach
//   HashMap_Remove each empty group safely
//   destroy_group + return IP to pool
```

---

## Bug 4 — gnome-terminal window not closing on leave/logout

**File:** `client/client_mng.c`, `client/mc_sender.c`, `client/mc_receiver.c`

**Symptom:** After leaving a group or logging out, the sender and receiver terminal windows stayed open.

**Root cause:** `kill(mc_sender_pid, SIGTERM)` killed `mc_sender` but `gnome-terminal` kept the window open showing "[Process completed]" by default.

**Fix:** Used `gnome-terminal --wait` so gnome-terminal closes the window automatically when its child exits. Combined with `&` to background it so `system()` returns immediately:
```c
"gnome-terminal --wait -- bash -c './mc_sender args' &"
```
Also added `close(STDIN_FILENO)` in mc_sender's SIGTERM handler to immediately unblock `fgets()`, and `close(g_sock)` in mc_receiver's SIGTERM handler to immediately unblock `recvfrom()`.

---

## Bug 5 — Killing ALL terminal windows instead of only the target group's windows

**File:** `client/mc_sender.c`, `client/mc_receiver.c`

**Symptom:** Leaving one group closed ALL terminal windows from all clients on the machine.

**Root cause:** `getppid()` was used to get the terminal's PID. But `gnome-terminal` uses a client-server model — `gnome-terminal-server` manages all windows as a shared parent process. `getppid()` always returned the shared server's PID. Killing it killed every window.

**Fix:** Stored each process's own PID via `getpid()`. Used `gnome-terminal --wait` to close the window when the child exits, without needing to kill any parent process.

---

## Bug 6 — Cross-group message leakage

**File:** `server/group_mng.c`

**Symptom:** Messages sent in one group appeared in a different group's receiver window.

**Root cause:** All groups shared the same multicast port (`MC_PORT_BASE = 5000`). Receivers bound to `INADDR_ANY:5000` received all multicast traffic on that port, regardless of which multicast IP they joined via `IP_ADD_MEMBERSHIP`.

**Fix:** Each group gets a unique port derived from its multicast IP's last octet:
```c
int last_octet = atoi(strrchr(group->mc_ip, '.') + 1);
group->mc_port  = (uint16_t)(MC_PORT_BASE + last_octet);
// 239.0.0.1 → port 5001
// 239.0.0.5 → port 5005
```

---

## Bug 7 — Shared message queue names between multiple clients

**File:** `client/client_mng.c`, `client/mc_sender.c`, `client/mc_receiver.c`

**Symptom:** When two clients ran on the same machine, the second client's terminal windows did not open, or PIDs were mixed between clients.

**Root cause:** Both clients used hardcoded queue names `/lanchat_rpid` and `/lanchat_spid`. When both tried to use the queues simultaneously, they read each other's PIDs.

**Fix:** Generate unique queue names per client using the client's PID:
```c
snprintf(mq_r_name, sizeof(mq_r_name), "/lanchat_r_%d", getpid());
snprintf(mq_s_name, sizeof(mq_s_name), "/lanchat_s_%d", getpid());
```
Queue names are then passed as command-line arguments to `mc_receiver` and `mc_sender`.

---

## Bug 8 — fgets() blocking prevents mc_sender from exiting on SIGTERM

**File:** `client/mc_sender.c`

**Symptom:** After leaving a group, the sender window took 30+ seconds to close and only closed after the user typed something in the sender window.

**Root cause:** The SIGTERM handler set `running = 0` but `fgets()` was blocked on stdin. The while loop condition `running && fgets(...)` was only re-evaluated after `fgets()` returned, which only happened when the user typed input.

**Fix:** Close stdin in the SIGTERM handler to immediately unblock `fgets()`:
```c
static void sigterm_handler(int sig)
{
    (void)sig;
    running = 0;
    close(STDIN_FILENO);  /* unblock fgets() immediately */
}
```

---

## Bug 9 — recvfrom() blocking prevents mc_receiver from exiting on SIGTERM

**File:** `client/mc_receiver.c`

**Symptom:** After leaving a group, the receiver window stayed open until a new multicast packet arrived.

**Root cause:** Same pattern as Bug 8 — `recvfrom()` blocked waiting for UDP packets. SIGTERM set `running = 0` but the blocking call didn't return until the next packet arrived.

**Fix:** Made the socket a global variable and closed it in the SIGTERM handler:
```c
static int g_sock = -1;

static void sigterm_handler(int sig)
{
    (void)sig;
    running = 0;
    if (g_sock >= 0) close(g_sock);  /* unblock recvfrom() immediately */
}
```
