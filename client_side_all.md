# Client Side — Complete Implementation Log

**Project:** LAN Chat (Embedded & RTOS)
**Date:** 2026-05-30
**Scope:** Every client-side file from scratch to fully wired Phase 4.

---

## File Status — Final

| File | Status |
|------|--------|
| `common/protocol.c/.h` | Complete — reviewed, no changes needed |
| `client/client_net.c/.h` | Complete |
| `client/ui.c/.h` | Complete + patched |
| `client/test_ui.c` | Complete — 16 tests, all pass |
| `client/client_mng.h` | Complete |
| `client/client_mng.c` | Complete — all 6 handlers + Phase 4 wired |
| `client/client_groups_mng.h` | Complete |
| `client/client_groups_mng.c` | Complete |
| `client/client_main.c` | Complete |
| `client/mc_sender.c` | Complete |
| `client/mc_receiver.c` | Complete |

---

## Part 1 — `protocol.c` Review

All three functions confirmed correct. No changes made.

| Function | What it does |
|----------|-------------|
| `tlv_encode` | Writes tag + 2-byte big-endian length + value into a buffer |
| `tlv_decode` | Reads one TLV — returns tag, zero-copy pointer to value, length |
| `tlv_find_field` | Walks a value block TLV-by-TLV searching for a specific field tag |

---

## Part 2 — `client_net.c`

### Purpose
The wire layer. Owns one TCP socket and one partial-TLV receive buffer. Every byte to/from the server passes through here.

### Phase 1 — `ClientNet_Create`

```c
ClientNet *ClientNet_Create(const char *server_ip, uint16_t port)
```

Steps:
1. `malloc` the struct
2. `socket(AF_INET, SOCK_STREAM, 0)` — TCP file descriptor
3. `inet_pton(AF_INET, server_ip, ...)` — converts IP string to binary, validates it
4. `htons(port)` — converts port to network byte order (big-endian)
5. `connect()` — TCP three-way handshake with server
6. Set `buf_len = 0` — receive buffer starts empty

Returns `NULL` on any failure. Never calls `exit()`.

### Phase 2 — `ClientNet_SendMsg`

```c
int ClientNet_SendMsg(ClientNet *net, const uint8_t *buf, uint16_t len)
```

Uses a write loop — `send()` may deliver fewer bytes than requested when the kernel send buffer is full. The loop advances `buf + sent` until all bytes are delivered.

### Phase 3 — `ClientNet_RecvMsg`

```c
int ClientNet_RecvMsg(ClientNet *net, uint8_t *out_buf, int out_size)
```

TCP is a byte stream — no message boundaries. This function accumulates bytes in `recv_buf` across multiple calls and returns exactly one complete TLV when ready.

**5-step algorithm:**

| Step | Action |
|------|--------|
| A | `recv()` appends bytes to tail of `recv_buf` |
| B | Check for at least 3 bytes (TLV header) |
| C | Read 2-byte length field with `ntohs()` (big-endian → host) |
| D | Check if all `3 + length` bytes are present |
| E | Copy complete TLV to `out_buf`, `memmove` buffer left to consume it |

Returns: byte count on success, `0` if not ready yet, `-1` on error.

**Critical: `ntohs()` for the length field**
```c
memcpy(&value_len, net->recv_buf + 1, sizeof(uint16_t));
value_len = ntohs(value_len);  /* big-endian network → host byte order */
```

### Phase 4 — `ClientNet_Destroy`

`close(sockfd)` sends TCP FIN (graceful shutdown). `free(net)`. NULL-safe.

---

## Part 3 — `ui.c` / `ui.h`

### Changes Made

**Added `MENU_INVALID = 0` and `GROUP_MENU_INVALID = 0`**

Before: returning `0` on bad input was a magic number.
After: it is a named enum value so `client_mng` can write `if (choice == MENU_INVALID)`.

```c
typedef enum {
    MENU_INVALID  = 0,
    MENU_REGISTER = 1,
    MENU_LOGIN,
    MENU_EXIT
} MenuOption;

typedef enum {
    GROUP_MENU_INVALID = 0,
    GROUP_MENU_CREATE  = 1,
    GROUP_MENU_JOIN,
    GROUP_MENU_LEAVE,
    GROUP_MENU_LOGOUT
} GroupMenuOption;
```

**Added `showMessage(const char *msg)`**

Without it, `client_mng` would call `printf` directly for server feedback, breaking the thin-layer principle.

```c
void showMessage(const char *msg) { printf("\n>> %s\n", msg); }
```

### Test Results (`test_ui.c`) — all 16 pass

```
[PASS] showMessage runs without crash
[PASS] showMenu / showGroupMenu run without crash
[PASS] choice 1/2/3 → correct enum values
[PASS] non-numeric input → MENU_INVALID
[PASS] choice 1/2/3/4 → correct GroupMenuOption values
[PASS] non-numeric input → GROUP_MENU_INVALID
[PASS] username parsed correctly, no trailing newline
[PASS] password parsed correctly, no trailing newline
[PASS] getCredentials(NULL) does not crash
[PASS] group name parsed correctly, no trailing newline
[PASS] getGroupName(NULL, ...) does not crash
[PASS] getGroupName(buf, 0) does not crash
```

**`set_stdin` fd-leak fix:** tracked previous `tmpfile()` with `static FILE *prev` and closes it before opening the next one.

---

## Part 4 — `client_mng.h`

### `ClientMng` struct — final form

```c
typedef struct {
    ClientNet       *net;                   /* TCP connection               */
    ClientGroupsMng *groups;                /* local group tracking         */
    char             username[USERNAME_SIZE]; /* stored on login, used by mc_sender */
    ClientState      state;                 /* SCREEN_1 or SCREEN_2         */
    int              running;               /* loop kill-switch             */
} ClientMng;
```

**`username` added in Phase 4** — stored when login succeeds, passed to `mc_sender` as `argv[3]` when launching chat windows. Without this field, `handle_create_group` and `handle_join_group` would have no way to know the logged-in username.

### Public API

| Function | Description |
|----------|-------------|
| `ClientMng_Create(ip, port)` | malloc, ClientNet_Create, ClientGroupsMng_Create, set state+running |
| `ClientMng_Run(mng)` | main while loop — blocks until exit |
| `ClientMng_Destroy(mng)` | ClientGroupsMng_Destroy → ClientNet_Destroy → free. NULL-safe |

---

## Part 5 — `client_mng.c`

### Architecture — the main loop

```c
while (mng->running) {
    if (mng->state == SCREEN_1) {
        showMenu();
        switch (getMenuChoice()) {
            MENU_REGISTER → handle_register
            MENU_LOGIN    → handle_login  (on success: state = SCREEN_2, store username)
            MENU_EXIT     → running = 0
        }
    } else {
        showGroupMenu();
        switch (getGroupMenuChoice()) {
            GROUP_MENU_CREATE  → handle_create_group
            GROUP_MENU_JOIN    → handle_join_group
            GROUP_MENU_LEAVE   → handle_leave_group
            GROUP_MENU_LOGOUT  → handle_logout  (on success: state = SCREEN_1)
        }
    }
}
```

### Phase 1 — Lifecycle

**`ClientMng_Create` — fail-safe chain:**
```c
mng->net = ClientNet_Create(...)
if (!mng->net) { free(mng); return NULL; }

mng->groups = ClientGroupsMng_Create()
if (!mng->groups) { ClientNet_Destroy(mng->net); free(mng); return NULL; }
```
If `groups` creation fails, `net` is destroyed first — no leak.

**`ClientMng_Destroy` — order matters:**
```c
ClientGroupsMng_Destroy(mng->groups);  /* kills child processes first */
ClientNet_Destroy(mng->net);           /* then closes TCP socket      */
free(mng);
```
Groups are destroyed before the socket because running child processes may still use the network.

### Phase 2 — Helpers

#### `send_and_recv`

```c
static int send_and_recv(ClientMng *mng,
                         uint8_t *send_buf, uint16_t send_len,
                         uint8_t *recv_buf, int recv_size)
```

Every handler sends a request and waits for a response. This centralises that logic.

The `do { n = ClientNet_RecvMsg(...); } while (n == 0)` loop is needed because `RecvMsg` returns `0` when bytes arrived but don't yet form a complete TLV. Since `recv()` is blocking, this is correct.

On any error: `showMessage("Error: lost connection")`, `mng->running = 0`, return `-1`.

#### `status_to_message`

Maps every `StatusCode` byte to a human-readable string. Every handler ends with one line: `showMessage(status_to_message(status_val[0]))`.

### Phase 2 — Screen 1 handlers

**Two-level TLV pattern used by register and login:**
```
inner buffer:  [ TAG_USERNAME TLV ][ TAG_PASSWORD TLV ]
                        ↓
outer buffer:  [ TAG_REGISTER_REQ ][ inner_len ][ inner buffer ]
```

`handle_login` additionally stores the username and flips `state = SCREEN_2` on success:
```c
strncpy(mng->username, cred.username, USERNAME_SIZE - 1);
mng->username[USERNAME_SIZE - 1] = '\0';
mng->state = SCREEN_2;
```

### Phase 3 — `handle_logout`

`LOGOUT_REQ` carries no payload — the server identifies the user by TCP `client_id`. Message is 3 bytes:
```
[ TAG_LOGOUT_REQ ][ 0x00 ][ 0x00 ]
```
On success: `ClientGroupsMng_RemoveAll` kills all chat windows, then `state = SCREEN_1`.

### Phase 4 — Group handlers

**`handle_create_group` / `handle_join_group`** — response has three fields:
```
[ TAG_STATUS  ][ 1  ][ code      ]
[ TAG_MC_IP   ][ len ][ "239.0.0.x" ]   ← NOT null-terminated in TLV
[ TAG_MC_PORT ][ 2  ][ big-endian  ]
```

**Critical — MC_IP null-termination:**
```c
/* WRONG — no null terminator in TLV value */
printf("%s", ip_val);

/* CORRECT */
char mc_ip[64];
memcpy(mc_ip, ip_val, ip_len);
mc_ip[ip_len] = '\0';
```

**Critical — MC_PORT byte order:**
```c
/* WRONG on little-endian x86 */
uint16_t mc_port;
memcpy(&mc_port, port_val, 2);

/* CORRECT — manually reconstruct big-endian */
uint16_t mc_port = ((uint16_t)port_val[0] << 8) | port_val[1];
```

After extracting MC info on success:
```c
ClientGroupsMng_Add(mng->groups, group_name, mc_ip, mc_port);
launch_chat_windows(mng, group_name, mc_ip, mc_port);
```

**`handle_leave_group`** — on success:
```c
ClientGroupsMng_Remove(mng->groups, group_name);
/* Remove kills PIDs internally — no kill() needed here */
```

---

## Part 6 — `client_groups_mng.c/.h`

### Why HashMap

| Operation | LinkedList | HashMap |
|-----------|------------|---------|
| Find by name | O(n) | O(1) |
| Remove by name | O(n) | O(1) |
| Add | O(1) | O(1) |
| Iterate all (logout) | O(n) | O(n) |

Primary operation is find-by-name — HashMap wins.

### `GroupEntry` struct

```c
typedef struct {
    char     group_name[GROUP_NAME_SIZE];
    char     mc_ip[MC_IP_SIZE];
    uint16_t mc_port;
    pid_t    sender_pid;    /* 0 until Phase 4 sets it */
    pid_t    receiver_pid;  /* 0 until Phase 4 sets it */
} GroupEntry;
```

PIDs start at `0` — on Linux `pid 0` = process group. Guard `if (pid > 0)` prevents calling `kill(0, SIGTERM)` which would signal the entire process group.

### Static helpers

**`hash_string` — djb2:**
```c
static size_t hash_string(void *key) {
    const char *str = (const char *)key;
    size_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + (size_t)c;
    return hash;
}
```

**`keys_equal` — strcmp wrapper:**
Without this, HashMap uses pointer equality — two different `char*` with the same content would be different keys.

**`kill_pids_action` — ForEach callback:**
```c
static int kill_pids_action(const void *key, void *value, void *context) {
    GroupEntry *entry = (GroupEntry *)value;
    if (entry->sender_pid > 0)   kill(entry->sender_pid,   SIGTERM);
    if (entry->receiver_pid > 0) kill(entry->receiver_pid, SIGTERM);
    return 1;  /* must return non-zero to continue iteration */
}
```

### Key operations

**`Add` — two heap allocations:**
- `strdup(group_name)` → the HashMap-owned key
- `malloc(GroupEntry)` → the HashMap-owned value

Both freed by `HashMap_Destroy(free, free)` or `HashMap_Remove`.

**`Remove` — `HashMap_Remove` returns both pointers:**
```c
HashMap_Remove(map, searchKey, &out_key, &out_value);
/* out_key   = the strdup'd copy — must be freed */
/* out_value = the GroupEntry   — must be freed  */
kill PIDs first, then free both.
```

**`RemoveAll` — destroy + recreate:**
```c
HashMap_ForEach(mng->map, kill_pids_action, NULL); /* kill all PIDs */
HashMap_Destroy(&mng->map, free, free);            /* free all entries */
mng->map = HashMap_Create(INITIAL_CAPACITY, ...);  /* fresh map for next session */
```
Manager struct stays alive — only its contents are cleared.

**`Find` — returns live pointer:**
Caller can write `entry->sender_pid = pid` directly. Never `free` the returned pointer.

---

## Part 7 — `client_main.c`

```c
int main(int argc, char *argv[])
{
    /* validate: argc == 3, port in 1-65535 */
    ClientMng *mng = ClientMng_Create(argv[1], (uint16_t)port_arg);
    if (!mng) { fprintf(stderr, "..."); return EXIT_FAILURE; }
    ClientMng_Run(mng);
    ClientMng_Destroy(mng);
    return EXIT_SUCCESS;
}
```

Three lines of logic. Every decision lives in `client_mng`.

---

## Part 8 — `mc_sender.c`

**Usage:** `./mc_sender <mc_ip> <mc_port> <username>`

### Phase 1 — PID report

```c
mqd_t mq = mq_open("/lanchat_spid", O_WRONLY);
pid_t my_pid = getpid();
mq_send(mq, (const char *)&my_pid, sizeof(pid_t), 0);
mq_close(mq);
```

Opens the queue `O_WRONLY` — sender only writes PIDs, never reads. Sends immediately and closes — queue no longer needed.

### Phase 2 — UDP socket + multicast TTL

```c
int sock = socket(AF_INET, SOCK_DGRAM, 0);  /* UDP, not TCP */

unsigned char ttl = 1;
setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
/* TTL=1: packet dies at first router, stays on LAN */

/* No bind() — sendto() targets the MC address directly */
struct sockaddr_in dest_addr;
dest_addr.sin_family = AF_INET;
dest_addr.sin_port   = htons(mc_port);
inet_pton(AF_INET, mc_ip, &dest_addr.sin_addr);
```

### Phase 3 — read-send loop + SIGTERM

```c
static volatile sig_atomic_t running = 1;
static void sigterm_handler(int sig) { (void)sig; running = 0; }

signal(SIGTERM, sigterm_handler);

while (running && fgets(line, sizeof(line), stdin) != NULL) {
    line[strcspn(line, "\n")] = '\0';       /* strip newline      */
    if (line[0] == '\0') continue;          /* skip empty lines   */
    snprintf(message, sizeof(message), "%s: %s", username, line);
    sendto(sock, message, strlen(message), 0,
           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
}
close(sock);
```

**`volatile sig_atomic_t`** — the only type safe to write from a signal handler and read from the main loop without a data race.

**Two exit paths:**
- `running == 0` — SIGTERM received, while condition fails
- `fgets()` returns `NULL` — stdin closed or signal interrupted

---

## Part 9 — `mc_receiver.c`

**Usage:** `./mc_receiver <mc_ip> <mc_port>`

**Note:** requires `#define _GNU_SOURCE` before any system header — `struct ip_mreq` is hidden behind this macro on glibc.

### Phase 1 — PID report

Identical to sender but opens `/lanchat_rpid` (receiver-specific queue).

### Phase 2 — UDP socket setup (4 steps)

```c
/* Step A: UDP socket */
int sock = socket(AF_INET, SOCK_DGRAM, 0);

/* Step B: allow multiple receivers on same port on same machine */
int reuse = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

/* Step C: bind to port on ALL interfaces — NOT to the MC IP */
struct sockaddr_in local_addr;
local_addr.sin_family      = AF_INET;
local_addr.sin_port        = htons(mc_port);
local_addr.sin_addr.s_addr = INADDR_ANY;
bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));

/* Step D: join the multicast group — THE key multicast call */
struct ip_mreq mreq;
inet_pton(AF_INET, mc_ip, &mreq.imr_multiaddr);
mreq.imr_interface.s_addr = INADDR_ANY;
setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
```

**Why `bind(INADDR_ANY)` not `bind(mc_ip)`?**
Bind to the port on all interfaces, then `IP_ADD_MEMBERSHIP` separately tells the kernel which group to deliver. Binding to the MC IP directly is unreliable across Linux implementations.

**Why `SO_REUSEADDR`?**
Multiple clients on the same machine can all bind to port `mc_port`. Without it, only one receiver per machine could listen.

**`IP_ADD_MEMBERSHIP` — without this, nothing works.**
Even if the packet arrives at the NIC, the kernel drops it unless the socket has joined the group.

### Phase 3 — recvfrom loop + SIGTERM + cleanup

```c
signal(SIGTERM, sigterm_handler);

while (running) {
    ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if (n < 0) {
        if (errno == EINTR) break;   /* normal SIGTERM exit path */
        perror("recvfrom"); break;
    }
    buf[n] = '\0';    /* null-terminate — TLV value has no \0 */
    printf("%s\n", buf);
    fflush(stdout);   /* force output — terminal is line-buffered */
}

/* Cleanup */
setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
close(sock);
```

**`errno == EINTR`** — when SIGTERM fires, `recvfrom()` is interrupted and returns `-1`. `EINTR` means "interrupted by signal" — this is the **normal exit path**, not an error. Without this check, the program would print an error and exit non-cleanly.

**`fflush(stdout)`** — terminal output is line-buffered. Without this, messages accumulate in the output buffer and may not appear until the buffer fills or the program exits.

**`IP_DROP_MEMBERSHIP`** — tells the kernel to stop delivering this group's packets to our socket. Required for clean shutdown.

---

## Part 10 — Phase 4 Wiring in `client_mng.c`

### `launch_chat_windows` — the full window launch sequence

```c
static void launch_chat_windows(ClientMng *mng, const char *group_name,
                                const char *mc_ip, uint16_t mc_port)
```

**Why two separate queues (`/lanchat_rpid` and `/lanchat_spid`)?**

If both sender and receiver sent to one queue, whichever process starts first sends first — we'd have no way to tell which PID is which. Two queues guarantee: receiver PID is always in `/lanchat_rpid`, sender PID always in `/lanchat_spid`.

**Full flow:**

```
Step 1 — Create both queues (O_CREAT | O_RDONLY):
   mq_attr: maxmsg=4, msgsize=sizeof(pid_t)
   /lanchat_rpid  ← for receiver PID
   /lanchat_spid  ← for sender PID

Step 2 — Launch mc_receiver:
   snprintf(cmd, "gnome-terminal -- ./mc_receiver %s %u", mc_ip, mc_port)
   system(cmd)   ← returns immediately (gnome-terminal forks)

Step 3 — Launch mc_sender:
   snprintf(cmd, "gnome-terminal -- ./mc_sender %s %u %s", mc_ip, mc_port, username)
   system(cmd)   ← returns immediately

Step 4 — Block until both PIDs arrive:
   mq_receive(mq_r, &receiver_pid, sizeof(pid_t), NULL)  ← blocks
   mq_receive(mq_s, &sender_pid,   sizeof(pid_t), NULL)  ← blocks
   No sleep() needed — mq_receive IS the wait mechanism.

Step 5 — Store PIDs in GroupEntry:
   GroupEntry *entry = ClientGroupsMng_Find(mng->groups, group_name)
   entry->receiver_pid = receiver_pid
   entry->sender_pid   = sender_pid

Step 6 — Cleanup:
   mq_close(mq_r) + mq_unlink("/lanchat_rpid")
   mq_close(mq_s) + mq_unlink("/lanchat_spid")
   Queues are single-use — unlink after each group join.
```

**Why `mq_open(O_CREAT | O_RDONLY)` and not `O_RDWR`?**
`client_mng` only ever reads from these queues. Using `O_RDONLY` makes the intent explicit and prevents accidental writes from the manager side.

**Why `mq_unlink` at the end?**
If the app crashes before unlink, the queue persists on the filesystem (`/dev/mqueue/`). Unlinking after each use ensures a clean state for the next join. On the next group join, `O_CREAT` creates it fresh.

### Changes to `client_mng.h`

```c
/* ui.h included for USERNAME_SIZE */
char username[USERNAME_SIZE];   /* stored on login, passed to mc_sender argv[3] */
```

### Changes to `handle_login`

```c
if ((StatusCode)status_val[0] == STATUS_SUCCESS) {
    strncpy(mng->username, cred.username, USERNAME_SIZE - 1);
    mng->username[USERNAME_SIZE - 1] = '\0';
    mng->state = SCREEN_2;
}
```

Username must be stored at login time — not at join time — because `getCredentials` is only called during login. By the time the user joins a group, the username is no longer being typed.

---

## Compile Check — All Files

```bash
# All commands: gcc -Wall -Wextra -g ... -c <file> -o /tmp/<name>.o
client_mng        OK   (includes mqueue.h, sys/stat.h)
mc_sender         OK
mc_receiver       OK   (_GNU_SOURCE required for struct ip_mreq)
client_groups_mng OK   (links against DS HashMap)
client_net        OK
client_main       OK
ui                OK
protocol          OK
```

Zero warnings across all eight files.

---

## Full Dependency Map

```
client_main
    └── ClientMng (Create / Run / Destroy)
            ├── client_net          TCP socket — all server comms
            │
            ├── client_groups_mng   HashMap — local group tracking
            │       └── libds.so    HashMap_Create/Insert/Find/Remove/ForEach/Destroy
            │
            ├── ui                  Terminal menus + input
            │
            ├── protocol            tlv_encode / tlv_decode / tlv_find_field
            │
            ├── mc_sender           (child process) stdin → UDP multicast
            │       └── /lanchat_spid  POSIX message queue (PID report)
            │
            └── mc_receiver         (child process) UDP multicast → stdout
                    └── /lanchat_rpid  POSIX message queue (PID report)
```

---

## What Still Needs Integration (Server Side)

The client is complete. Integration testing requires Raz's server-side modules:

| Server Module | Status |
|---------------|--------|
| `server_net.c` | Raz's responsibility |
| `server_mng.c` | Raz's responsibility |
| `user_mng.c` | Raz's responsibility |
| `group_mng.c` | Raz's responsibility |

Once the server is ready: build both sides, run `./chat_server 9000` on one terminal and `./chat_client 127.0.0.1 9000` on another to begin Phase 5 integration testing.