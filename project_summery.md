# LanChat — Deep Project Summary

**Date:** 2026-05-31  
**Language:** C (C99/GNU)  
**Authors:** Ezra (client side) + Raz (server side)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Architecture](#2-architecture)
3. [The TLV Protocol — Deep Dive](#3-the-tlv-protocol--deep-dive)
4. [Server Side — Deep Dive](#4-server-side--deep-dive)
5. [Client Side — Deep Dive](#5-client-side--deep-dive)
6. [How Server and Client Work Together](#6-how-server-and-client-work-together)
7. [UDP Multicast — How Chat Actually Works](#7-udp-multicast--how-chat-actually-works)
8. [Key Concepts Explained](#8-key-concepts-explained)
9. [How Bad Code Breaks Things](#9-how-bad-code-breaks-things)
10. [Best Practices Review](#10-best-practices-review)

---

## 1. Project Overview

LanChat is a real-time group chat application for a Local Area Network (LAN). It is written entirely in C and split into two executables — a **server** and a **client**.

The core design is split across two transport protocols with very different jobs:

| Transport | Used For | Why |
|-----------|----------|-----|
| **TCP** | Registration, login, logout, group create/join/leave | Needs reliability — these are control commands. A lost packet means the user never logs in. |
| **UDP Multicast** | Actual chat messages | Speed over reliability. A missed chat line is acceptable. Multicast allows one send to reach many receivers simultaneously. |

The server's **only job** is user and group management. It does not see or relay chat messages at all. When a group is created, the server assigns a multicast IP address and tells all joining clients what it is. The clients then talk directly to each other via UDP on that address — the server is completely out of the chat path.

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         SERVER PROCESS                           │
│                                                                  │
│  server_main.c                                                   │
│       │                                                          │
│       └──► ServerMng ◄─────────────────────────────────────┐    │
│              │  │                                           │    │
│              │  ├──► UserMng ──► UserHash (HashMap)         │    │
│              │  │        └──► User structs                  │    │
│              │  │                                           │    │
│              │  ├──► GroupMng ──► GroupHash (HashMap)       │    │
│              │  │        └──► Group structs                 │    │
│              │  │             └──► members (LinkedList)     │    │
│              │  │        └──► FreeMcIpQueue (GenQueue)      │    │
│              │  │                                           │    │
│              │  └──► ServerNet ───────────────────────────► │    │
│                         │     TCP select() loop             │    │
│                         │     ClientNode LinkedList         │    │
└─────────────────────────┼────────────────────────────────────────┘
                          │ TCP (port 9000)
                          │ Request/Response — TLV encoded
                          │
┌─────────────────────────┼────────────────────────────────────────┐
│                         │      CLIENT PROCESS                    │
│                         │                                        │
│  client_main.c          │                                        │
│       │                 │                                        │
│       └──► ClientMng ◄──┘                                        │
│              │                                                   │
│              ├──► ClientNet (TCP socket + recv buffer)           │
│              ├──► ClientGroupsMng (HashMap of GroupEntry)        │
│              └──► UI (terminal menus, input, output)             │
│                                                                  │
│  On group join/create:                                           │
│    ├── gnome-terminal ──► mc_receiver process                   │
│    │        └── UDP multicast recvfrom() loop                   │
│    └── gnome-terminal ──► mc_sender process                     │
│             └── reads stdin → UDP multicast sendto()            │
│                                                                  │
│    PIDs communicated back via POSIX message queues               │
└──────────────────────────────────────────────────────────────────┘
                          │
          UDP Multicast (239.0.0.x:5000)
          One send → all receivers on LAN
                          │
              ┌───────────┴───────────┐
          Client A                Client B
        mc_receiver             mc_receiver
        mc_sender               mc_sender
```

---

## 3. The TLV Protocol — Deep Dive

### 3.1 What is TLV and Why Use It?

TLV stands for **Tag-Length-Value**. It is a binary serialization format — a way of packing structured data into a flat byte stream that can be sent over a network.

Every piece of data in this application is sent as one or more TLV units. The layout of each unit on the wire is:

```
Byte 0       Bytes 1-2           Bytes 3 to 3+Length-1
┌───────────┬─────────────────┬────────────────────────┐
│  Tag      │  Length         │  Value                 │
│  (1 byte) │  (2 bytes, BE)  │  (Length bytes)        │
└───────────┴─────────────────┴────────────────────────┘
```

- **Tag** (1 byte): identifies what kind of data this is. E.g. `0x01` = REGISTER_REQ, `0x20` = USERNAME.
- **Length** (2 bytes, big-endian): how many value bytes follow. Can be 0 to 65535.
- **Value** (Length bytes): the actual data — a string, a number, or another nested TLV.

**Why TLV instead of plain text?**
- Fixed-width fields would waste space (what if the username is only 3 chars?).
- Delimited text (like HTTP headers with `\r\n`) is ambiguous if data contains the delimiter.
- TLV is self-describing — a receiver can always find the boundary of one field and advance to the next.
- TLV is extensible — adding a new field (new tag) doesn't break existing parsers.

### 3.2 The Tags

**Outer (message-level) tags — identify what the message IS:**

| Hex | Name | Direction |
|-----|------|-----------|
| 0x01 | TAG_REGISTER_REQ | Client → Server |
| 0x02 | TAG_REGISTER_RESP | Server → Client |
| 0x03 | TAG_LOGIN_REQ | Client → Server |
| 0x04 | TAG_LOGIN_RESP | Server → Client |
| 0x05 | TAG_LOGOUT_REQ | Client → Server |
| 0x06 | TAG_LOGOUT_RESP | Server → Client |
| 0x07 | TAG_CREATE_GROUP_REQ | Client → Server |
| 0x08 | TAG_CREATE_GROUP_RESP | Server → Client |
| 0x09 | TAG_JOIN_GROUP_REQ | Client → Server |
| 0x0A | TAG_JOIN_GROUP_RESP | Server → Client |
| 0x0B | TAG_LEAVE_GROUP_REQ | Client → Server |
| 0x0C | TAG_LEAVE_GROUP_RESP | Server → Client |

**Inner (field-level) tags — identify a specific piece of data inside a message:**

| Hex | Name | Content |
|-----|------|---------|
| 0x20 | TAG_USERNAME | UTF-8 string (not null-terminated in TLV) |
| 0x21 | TAG_PASSWORD | UTF-8 string |
| 0x22 | TAG_GROUP_NAME | UTF-8 string |
| 0x23 | TAG_MC_IP | IPv4 string e.g. "239.0.0.5" |
| 0x24 | TAG_MC_PORT | 2 bytes, big-endian |
| 0x25 | TAG_STATUS | 1 byte (0x00 = success, 0x01–0x09 = error codes) |
| 0x26 | TAG_ERROR_MSG | UTF-8 string (currently unused) |

### 3.3 Two-Level Nesting

Messages are **nested TLVs**. The outer TLV's value is itself a sequence of inner TLVs. Example — a REGISTER_REQ for user "alice" with password "secret":

```
Wire bytes (hex):
01 00 1A                    ← outer: tag=REGISTER_REQ, length=26
  20 00 05 61 6C 69 63 65   ← inner: tag=USERNAME, length=5, value="alice"
  21 00 06 73 65 63 72 65 74← inner: tag=PASSWORD, length=6, value="secret"
```

Decoded:
```
Outer TLV:
  tag    = 0x01  (TAG_REGISTER_REQ)
  length = 26
  value  = [ username TLV ] [ password TLV ]
    Inner TLV 1:
      tag    = 0x20  (TAG_USERNAME)
      length = 5
      value  = "alice"
    Inner TLV 2:
      tag    = 0x21  (TAG_PASSWORD)
      length = 6
      value  = "secret"
```

A REGISTER_RESP looks like:

```
02 00 04               ← outer: tag=REGISTER_RESP, length=4
  25 00 01 00          ← inner: tag=STATUS, length=1, value=0x00 (SUCCESS)
```

### 3.4 The Critical TCP Stream Problem

TCP is a **stream protocol**. There are no message boundaries. When you call `send(sock, buf, 100, 0)`, the kernel may deliver all 100 bytes at once, or in chunks of 30+40+30, or as a single byte at a time. The receiver has no way to know where one TLV ends and the next begins just from the raw bytes.

**What happens without a buffer:**

```
Sender calls: send(sock, tlv_20_bytes, 20, 0)

Receiver's recv():
  First call  → returns 12 bytes (first 12 of the TLV)
  Second call → returns 8 bytes  (remaining 8)
  
If the receiver passes 12 bytes to the TLV parser:
  tag    = valid
  length = 0x00 0x09 = 9 bytes expected
  value  = only 9 bytes available? NO — only 9 of the 12 bytes remain after the header
  But wait — 12 - 3 (header) = 9 bytes of value available
  So the first 12 bytes look like a complete TLV? NO — the length says 9 but the TLV
  needs 3+9=12 bytes total. Is 12 >= 12? Yes. 
  
  But if length had been 0x00 0x0F = 15 bytes:
  Total needed = 3+15=18. We only have 12. → Incomplete TLV → must wait!
```

**The solution — a per-connection receive buffer:**

Each connection (each `ClientNode` on the server, and the `ClientNet` on the client) owns a private `recv_buf[]` array. The algorithm every time bytes arrive:

```
Step A: recv() → append new bytes to the END of recv_buf, increment buf_len

Step B: Do we have at least 3 bytes (the TLV header)?
        If not → return 0 (need more data, try again later)

Step C: Read bytes [1] and [2] of recv_buf to get the length field.
        Apply ntohs() to convert from network (big-endian) to host byte order.
        total_needed = 3 + length

Step D: Is buf_len >= total_needed?
        If not → return 0 (TLV is incomplete, wait for more bytes)

Step E: We have a complete TLV!
        Copy [0 .. total_needed-1] to the caller's output buffer.
        Shift remaining bytes to the front: memmove(buf, buf+total_needed, buf_len-total_needed)
        buf_len -= total_needed
        Return total_needed
```

**The ntohs() call is critical.** Network byte order is big-endian. Most modern computers (x86/x64) are little-endian. Without `ntohs()`, a 2-byte length stored as `0x00 0x0F` (= 15 in big-endian) would be read as `0x0F00` (= 3840 in little-endian). The receiver would wait for 3843 bytes that never arrive — deadlock.

### 3.5 The Three Protocol Functions in `protocol.c`

**`tlv_encode(buf, buf_size, tag, value, value_len)`**
- Writes tag at `buf[0]`.
- Writes `htons(value_len)` at `buf[1..2]`.
- Copies `value` to `buf[3..]`.
- Returns total bytes written, or -1 if buf is too small.

**`tlv_decode(buf, buf_len, &out_tag, &out_value, &out_value_len)`**
- Reads tag from `buf[0]`.
- Reads and `ntohs()`-converts length from `buf[1..2]`.
- Sets `*out_value = buf + 3` (zero-copy pointer — no allocation!).
- Returns total bytes consumed.

**`tlv_find_field(value_block, block_len, field_tag, &out_value, &out_value_len)`**
- Walks the value block TLV by TLV (calling `tlv_decode` in a loop).
- When it finds a TLV with `tag == field_tag`, sets the output pointers and returns 0.
- Returns -1 if not found.
- This is how the server extracts USERNAME and PASSWORD from inside a REGISTER_REQ.

---

## 4. Server Side — Deep Dive

### 4.1 `server_main.c` — Entry Point

The simplest file. It:
1. Validates `argc` (needs a port argument).
2. Calls `ServerMng_Create(port)` — initializes everything.
3. Calls `ServerMng_Run()` — blocks in the event loop until shutdown.
4. Calls `ServerMng_Destroy()` — cleans up all resources.

```c
int main(int argc, char *argv[])
{
    int port = atoi(argv[1]);
    if (ServerMng_Create(port) < 0) return 1;
    printf("Server listening on port %d\n", port);
    ServerMng_Run();
    ServerMng_Destroy();
    return 0;
}
```

**Design principle:** `main` has no logic. It is just a thin shell that delegates everything to `ServerMng`. This means the server behavior can be tested without a `main` by calling the same three functions from a test harness.

### 4.2 `server_net.c` — The TCP Event Loop

This is the network foundation of the server. It has **no threads**. A single `select()` loop handles all connected clients simultaneously.

#### The ClientNode Structure

Every connected TCP client is represented by a node:

```c
typedef struct ClientNode {
    int     socket_fd;             /* the OS file descriptor for this TCP connection */
    int     client_id;             /* unique integer ID assigned at accept time */
    uint8_t recv_buf[4096];        /* partial TLV reassembly buffer */
    int     buf_len;               /* bytes currently in recv_buf */
} ClientNode;
```

All nodes are stored in a **doubly linked list** (`s_clients`). The list is provided by `libds.so` (the pre-built data structures library).

#### Why a Doubly Linked List?

- **O(1) insertion:** New clients are added to the head of the list with `ListPushHead`.
- **O(1) removal:** When a client disconnects, `ListItrRemove(itr)` removes it in constant time because the iterator gives direct access to that node and the doubly-linked structure allows unlinking without walking from the head.
- **Sequential scan for select():** Building the `fd_set` requires visiting every connected client anyway, so O(n) traversal is acceptable here.

#### The `select()` Event Loop — How It Works

`select()` is the heart of the server. It watches multiple file descriptors (the listening socket + all client sockets) and tells you which ones have data ready to read, without blocking forever on any one of them.

```
Each iteration of the loop:

1. build_fd_set():
   - FD_ZERO(&read_fds)          ← clear the set
   - FD_SET(listen_fd, &read_fds)← always watch for new connections
   - For each ClientNode:
       FD_SET(node->socket_fd, &read_fds)
       track max_fd = highest fd value
   - Returns max_fd

2. select(max_fd+1, &read_fds, NULL, NULL, NULL)
   - Blocks until at least one fd is ready
   - On return, read_fds is modified — only ready fds remain set

3. if FD_ISSET(listen_fd, &read_fds):
   → accept_new_client()
     - accept() → new socket fd
     - malloc a new ClientNode
     - ListPushHead to add it to the list
     - s_next_id++ for the unique ID

4. process_all_clients(&read_fds):
   - Iterate the ClientNode list
   - For each node where FD_ISSET(node->socket_fd) is true:
     → recv() bytes into node->recv_buf + node->buf_len
     → if recv returns 0 or error:
         s_on_disconnect(node->client_id)  ← tell ServerMng
         close(node->socket_fd)
         ListItrRemove(current)
         free(node)
     → else:
         node->buf_len += bytes_received
         handle_client_data(node)  ← try to parse complete TLVs

5. handle_client_data(node):
   - While buf_len >= 3 (have a header):
     - Read length from buf[1..2], apply ntohs()
     - total = 3 + length
     - If buf_len < total: break (incomplete TLV, wait)
     - Else: fire s_on_message(client_id, buf, total)
             memmove to consume the TLV from the buffer
             buf_len -= total
             Loop (check for another TLV in the buffer)
```

**Why `max_fd + 1`?** The first argument to `select()` is not the number of fds — it is `max_fd + 1`. Internally, select() iterates from fd 0 to `nfds-1`. You must pass the highest fd you care about, plus 1, so select() checks all relevant fds.

**Why rebuild fd_set every iteration?** `select()` modifies the `fd_set` in place — after it returns, only the READY fds are still set. So you must rebuild the set fresh each time from the current list of clients.

#### Callbacks — Decoupling Net from Mng

`ServerNet` does not know what the messages mean. When a complete TLV arrives, it calls `s_on_message(client_id, buf, len)`. When a client disconnects, it calls `s_on_disconnect(client_id)`. These are **function pointers** (callbacks) registered at creation time:

```c
int ServerNet_Create(int port, OnMessageCb on_msg, OnDisconnectCb on_disc)
```

`ServerMng` passes its own static functions as callbacks. This decouples the network layer (how bytes arrive) from the management layer (what those bytes mean).

### 4.3 `server_mng.c` — The Brain

`ServerMng` is the central dispatcher. It sits between `ServerNet` (raw bytes) and the data managers (`UserMng`, `GroupMng`).

#### Message Routing

When `on_message(client_id, msg, msg_len)` fires:
1. `tlv_decode(msg, ...)` extracts the outer tag.
2. A `switch` on the tag dispatches to the correct handler.

```c
switch (tag) {
    case TAG_REGISTER_REQ:     handle_register(client_id, val, val_len);     break;
    case TAG_LOGIN_REQ:        handle_login(client_id, val, val_len);         break;
    case TAG_LOGOUT_REQ:       handle_logout(client_id);                      break;
    case TAG_CREATE_GROUP_REQ: handle_create_group(client_id, val, val_len);  break;
    case TAG_JOIN_GROUP_REQ:   handle_join_group(client_id, val, val_len);    break;
    case TAG_LEAVE_GROUP_REQ:  handle_leave_group(client_id, val, val_len);   break;
}
```

#### Session Mapping

`ServerMng` uses `client_id` (assigned by `ServerNet`) as the session identifier. The session is maintained inside `UserMng` via a second hash map (`s_user_hash_by_client_id`) that maps the string representation of `client_id` → `User*`.

This means:
- A TCP connection has a `client_id` (always, even before login).
- A `User` record has a `client_id` field (set to -1 when not logged in, set to the live connection's ID when logged in).
- When a request comes in, the server looks up `client_id` → `User*` to know who sent it.

#### Handlers

**REGISTER handler:**
```
Extract username + password from inner TLVs (tlv_find_field)
null-terminate both strings (TLV values are NOT null-terminated)
Call UserMng_Register(username, password)
  → If username already exists: return STATUS_USERNAME_ALREADY_EXISTS
  → If not: create User struct, insert into hash, return STATUS_SUCCESS
Send REGISTER_RESP with the status code
```

**LOGIN handler:**
```
Extract username + password
Call UserMng_Login(username, password, client_id)
  → If username not found: STATUS_USERNAME_NOT_FOUND
  → If password wrong: STATUS_WRONG_PASSWORD
  → If already active: STATUS_ALREADY_LOGGED_IN
  → Else: mark user active, store client_id, insert into client_id hash → STATUS_SUCCESS
Send LOGIN_RESP with the status code
```

**CREATE_GROUP handler:**
```
Extract group name
Call GroupMng_CreateGroup(group_name, client_id, &mc_ip, &mc_port)
  → Check if group already exists (GROUP_ALREADY_EXISTS)
  → Dequeue a free multicast IP from the pool
  → Create Group struct, set mc_port=5000, member_count=1
  → Add creator's client_id to the members list
  → Insert group into the Group hash
  → Return STATUS_SUCCESS with mc_ip and mc_port filled
Send CREATE_GROUP_RESP with STATUS + MC_IP + MC_PORT
```

**LOGOUT handler:**
```
GroupMng_RemoveClientFromAll(client_id)
  → For every group, remove client from member list, decrement counter
  → If any group becomes empty: destroy it, return IP to pool
UserMng_LogoutByClientId(client_id)
  → Set user.is_active = 0, user.client_id = -1
  → Remove from client_id hash map
Send LOGOUT_RESP with STATUS_SUCCESS
```

**Disconnect handler (no response sent — client is gone):**
```
GroupMng_RemoveClientFromAll(client_id)
UserMng_LogoutByClientId(client_id)
```

### 4.4 `user_mng.c` — User Records

`UserMng` manages two hash tables:

```
s_user_hash_by_name:
  key   = user->username (string stored inside the User struct)
  value = User* pointer
  Used for: register (uniqueness check), login (password validation)

s_user_hash_by_client_id:
  key   = string representation of client_id (e.g. "3")
  value = User* pointer
  Used for: per-message dispatch, logout-by-connection
```

The `User` struct:
```c
typedef struct {
    char username[64];   /* stored inside struct — key for name hash */
    char password[64];   /* compared at login */
    int  is_active;      /* 0 = logged out, 1 = logged in */
    int  client_id;      /* -1 when logged out, live ID when active */
} User;
```

**Why two hash maps?** Different lookup patterns need different keys:
- "Is this username taken?" → look up by name (O(1) with the name hash).
- "Who is client #7?" → look up by client_id (O(1) with the client_id hash).

Without the second map, every message would require a linear scan of all users to find the one with `client_id == 7`. With a hash map, it's constant time.

### 4.5 `group_mng.c` — Groups and the Multicast IP Pool

**The Free MC IP Queue:**

At startup, `init_mc_ip_pool()` fills the queue with 254 strings:
```
"239.0.0.1", "239.0.0.2", ..., "239.0.0.254"
```

Each is a `malloc`'d heap string. The queue is a FIFO — `QueueRemove` takes from the front, `QueueInsert` adds to the back.

When a group is **created**, one IP is dequeued. When the group is **destroyed** (last member leaves), the IP string is `strdup`'d and re-enqueued. This ensures each active group has a unique multicast IP and IPs are reused across the server's lifetime.

**The Group struct:**
```c
typedef struct {
    char     group_name[64];
    char     mc_ip[32];        /* e.g. "239.0.0.5" */
    uint16_t mc_port;          /* fixed at 5000 */
    int      member_count;     /* number of connected clients currently in group */
    List    *members;          /* linked list of int* client_ids */
} Group;
```

**The member list:** Each `int* client_id` in the list is a separately heap-allocated `malloc(sizeof(int))`. This is necessary because the LinkedList stores `void*` pointers — you cannot store a stack `int` directly. The `int` must live on the heap so the pointer remains valid.

**Auto-destroy logic:** In `remove_client_from_group`, after decrementing `member_count`:
```c
if (group->member_count <= 0) {
    char *ip_copy = strdup(group->mc_ip);       /* copy before destroying */
    HashMap_Remove(s_group_hash, ...);           /* remove from hash */
    destroy_group(group);                        /* free List + Group struct */
    QueueInsert(s_free_mc_ip_queue, ip_copy);   /* return IP to pool */
}
```

---

## 5. Client Side — Deep Dive

### 5.1 `client_main.c` — Entry Point

Validates arguments, then:
```c
ClientMng *mng = ClientMng_Create(server_ip, port);
ClientMng_Run(mng);     /* blocks here until user exits */
ClientMng_Destroy(mng);
```

### 5.2 `client_net.c` — The TCP Connection

`ClientNet` owns the single TCP socket and the receive buffer.

```c
typedef struct {
    int     sockfd;
    uint8_t recv_buf[1024];
    int     buf_len;
} ClientNet;
```

**`ClientNet_Create`:** Calls `socket()`, `inet_pton()` to parse the server IP, `htons()` for the port, then `connect()` for the TCP three-way handshake.

**`ClientNet_SendMsg` — the write loop:**
```c
uint16_t sent = 0;
while (sent < len) {
    ssize_t n = send(net->sockfd, buf + sent, len - sent, 0);
    if (n <= 0) return -1;
    sent += (uint16_t)n;
}
```
This loop is critical. `send()` can return less than `len` (partial write) without error. Without this loop, a partial write would silently truncate the TLV, causing the server to receive an incomplete message and desync the protocol.

**`ClientNet_RecvMsg` — the 5-step TLV reassembly:**
Identical in concept to the server's `handle_client_data`. Steps A–E as described in section 3.4.

Unlike the server which uses `select()` in a loop, the client calls `RecvMsg` from `ClientMng` in a busy-wait `do { n = ClientNet_RecvMsg(...); } while (n == 0)`. This is acceptable for the client because the client only sends one request at a time and waits for exactly one response before doing anything else. There is no concurrent client handling needed.

### 5.3 `client_mng.c` — The State Machine

`ClientMng` is the control center of the client application. Its central `Run` loop implements a **two-state machine**:

```
State: SCREEN_1 (pre-login)
┌────────────────────────────────────────────────┐
│  Show main menu                                │
│  Read user choice                              │
│  MENU_REGISTER  → handle_register()            │
│  MENU_LOGIN     → handle_login()               │
│    └── on SUCCESS: state = SCREEN_2            │
│  MENU_EXIT      → running = 0 → loop exits     │
└────────────────────────────────────────────────┘

State: SCREEN_2 (post-login)
┌────────────────────────────────────────────────┐
│  Show group menu                               │
│  Read user choice                              │
│  GROUP_MENU_CREATE  → handle_create_group()    │
│  GROUP_MENU_JOIN    → handle_join_group()      │
│  GROUP_MENU_LEAVE   → handle_leave_group()     │
│  GROUP_MENU_LOGOUT  → handle_logout()          │
│    └── on SUCCESS: state = SCREEN_1            │
└────────────────────────────────────────────────┘
```

**The `send_and_recv` helper:**
Every handler follows the same two steps — send a request, get a response. This helper centralizes both:
```c
static int send_and_recv(ClientMng *mng,
                         uint8_t *send_buf, uint16_t send_len,
                         uint8_t *recv_buf, int recv_size)
{
    if (ClientNet_SendMsg(mng->net, send_buf, send_len) < 0) {
        showMessage("Error: lost connection to server.");
        mng->running = 0;
        return -1;
    }
    int n;
    do {
        n = ClientNet_RecvMsg(mng->net, recv_buf, recv_size);
    } while (n == 0);    /* keep trying until a complete TLV arrives */
    if (n < 0) { mng->running = 0; return -1; }
    return n;
}
```

The `do-while` on `RecvMsg` is the client's equivalent of the server's select loop. The client calls `recv()` until a complete TLV is assembled in the buffer. Unlike the server which must handle many clients simultaneously, the client only ever waits for its one server response.

**Building a request — two-level TLV construction:**

For REGISTER_REQ (username "alice", password "secret"):
```c
/* Step 1: encode inner fields back-to-back */
uint8_t inner[MAX_TLV_SIZE];
int inner_len = 0;

inner_len += tlv_encode(inner + inner_len, ..., TAG_USERNAME, "alice", 5);
inner_len += tlv_encode(inner + inner_len, ..., TAG_PASSWORD, "secret", 6);

/* Step 2: wrap in outer tag */
uint8_t msg[MAX_TLV_SIZE];
int msg_len = tlv_encode(msg, sizeof(msg), TAG_REGISTER_REQ, inner, inner_len);

/* Step 3: send */
send_and_recv(mng, msg, msg_len, resp, sizeof(resp));
```

**Parsing a response — finding the status:**
```c
/* Step 1: decode outer TLV to get the value block */
uint8_t out_tag;
const uint8_t *value;
uint16_t value_len;
tlv_decode(resp, resp_len, &out_tag, &value, &value_len);

/* Step 2: find TAG_STATUS inside the value block */
const uint8_t *status_val;
uint16_t status_len;
tlv_find_field(value, value_len, TAG_STATUS, &status_val, &status_len);

/* Step 3: read the status byte */
StatusCode code = (StatusCode)status_val[0];
```

**On create/join group — extracting MC_IP and MC_PORT:**
```c
/* MC_IP is a UTF-8 string — NOT null-terminated in TLV */
const uint8_t *ip_val; uint16_t ip_len;
tlv_find_field(value, value_len, TAG_MC_IP, &ip_val, &ip_len);

char mc_ip[64];
memcpy(mc_ip, ip_val, ip_len);
mc_ip[ip_len] = '\0';    /* must null-terminate manually! */

/* MC_PORT is 2 bytes in network (big-endian) byte order */
const uint8_t *port_val; uint16_t port_field_len;
tlv_find_field(value, value_len, TAG_MC_PORT, &port_val, &port_field_len);

uint16_t mc_port = ((uint16_t)port_val[0] << 8) | port_val[1];
/* ntohs() equivalent — port_val[0] is the high byte */
```

**Why manually reconstruct the port instead of using `ntohs`?**
`port_val` is a `uint8_t*` — an unaligned pointer into the middle of a TLV buffer. Casting it directly to `uint16_t*` and calling `ntohs()` could cause an **alignment fault** on architectures that require 2-byte values to be at even addresses. The manual byte shift is portable and safe on all architectures.

### 5.4 `ui.c` — Terminal Interface

A thin layer. No protocol logic. Functions:
- `showMenu()` / `showGroupMenu()` — print the menus.
- `getMenuChoice()` / `getGroupMenuChoice()` — read an integer from stdin using `scanf`, then `clearInputBuffer()` to discard the trailing newline.
- `getCredentials(cred)` — read username and password using `fgets`, strip the newline with `strcspn`.
- `getGroupName(buf, size)` — read a group name using `fgets`.
- `showMessage(msg)` — print a result or error.

**Why `fgets` not `scanf` for strings?** `scanf("%s")` stops at whitespace — a username containing a space would be truncated. `fgets` reads the entire line including spaces, making it safe for any input.

**Why `clearInputBuffer` after `scanf`?** `scanf("%d")` reads a number but leaves the newline character `\n` in the input buffer. The next `fgets` call would immediately read that leftover newline as an empty line without waiting for user input. `clearInputBuffer` reads and discards everything up to and including the newline.

### 5.5 `client_groups_mng.c` — Local Group Tracking

The client maintains its own record of every group it is currently in. This is needed to:
1. Know which groups to kill windows for on logout.
2. Retrieve PIDs when a leave/logout happens.
3. Store the multicast IP/port for each group.

**The `GroupEntry` struct:**
```c
typedef struct {
    char     group_name[50];
    char     mc_ip[64];
    uint16_t mc_port;
    pid_t    sender_pid;    /* PID of the mc_sender terminal process */
    pid_t    receiver_pid;  /* PID of the mc_receiver terminal process */
} GroupEntry;
```

**Storage:** A `HashMap` from `libds.so`. Key = `strdup`'d group name (heap string). Value = `malloc`'d `GroupEntry`.

**Key design decision — `strdup` for the key:**
The HashMap stores a raw `void*` pointer as the key. If you pass a stack buffer address as the key, the HashMap holds a dangling pointer after the function returns. `strdup` makes a heap copy that persists as long as the entry lives. On removal, `HashMap_Remove` returns both the key and value pointers so both can be freed.

**The opaque struct pattern:**
```c
/* client_groups_mng.h — only the typedef, no struct body */
typedef struct ClientGroupsMng ClientGroupsMng;

/* client_groups_mng.c — actual definition hidden here */
struct ClientGroupsMng { HashMap *map; };
```

Callers only hold a `ClientGroupsMng*`. They cannot access `->map` directly. This forces all HashMap operations to go through the official API, preventing callers from bypassing cleanup logic (like PID killing on remove).

---

## 6. How Server and Client Work Together

### 6.1 Connection Establishment

```
Client:                          Server:
  socket()                         accept() → new socket fd
  connect() ──── TCP SYN ────►     accept() returns client_fd
                 TCP SYN-ACK ◄──   malloc ClientNode
  connect() returns                ListPushHead(s_clients, node)
  ClientNet_Create returns
```

After connect, the server assigns `client_id = s_next_id++`. The client doesn't know this ID — it identifies itself by the socket connection.

### 6.2 Registration Flow

```
User types "1" (Register)
  ↓
getCredentials() → username="alice", password="abc123"
  ↓
Client builds TLV:
  [0x01][0x00 0x14]                    ← REGISTER_REQ, 20 bytes of value
    [0x20][0x00 0x05][alice]           ← USERNAME, 5 chars
    [0x21][0x00 0x06][abc123]          ← PASSWORD, 6 chars
  ↓
ClientNet_SendMsg → TCP bytes to server
  ↓
ServerNet: select() fires on that client's fd
           recv() into ClientNode.recv_buf
           handle_client_data(): TLV complete → fires on_message callback
  ↓
ServerMng: tag = 0x01 → handle_register()
           tlv_find_field(TAG_USERNAME) → "alice"
           tlv_find_field(TAG_PASSWORD) → "abc123"
           UserMng_Register("alice", "abc123")
             → HashMap_Find(name_hash, "alice") → not found
             → calloc User, strncpy username+password
             → HashMap_Insert(name_hash, user->username, user)
             → return STATUS_SUCCESS
  ↓
ServerMng builds response:
  [0x02][0x00 0x04]                    ← REGISTER_RESP, 4 bytes of value
    [0x25][0x00 0x01][0x00]            ← STATUS, 1 byte, 0x00 = SUCCESS
  ↓
ServerNet_SendMsg → TCP bytes to client
  ↓
ClientNet_RecvMsg: assembles complete TLV in recv_buf
send_and_recv loop exits (n > 0)
  ↓
Client: tlv_decode → tag=0x02, value block
        tlv_find_field(TAG_STATUS) → status_val[0] = 0x00
        showMessage("Success!")
```

### 6.3 Login Flow

Identical to registration up to the handler. On success:
```
UserMng_Login():
  → find user by name
  → compare password
  → user->is_active = 1
  → user->client_id = client_id
  → snprintf(id_key, ..., client_id)
  → HashMap_Insert(client_id_hash, id_key, user)

Client on SUCCESS response:
  → strncpy(mng->username, cred.username, ...)
  → mng->state = SCREEN_2       ← transitions to the group menu
```

### 6.4 Create Group Flow

```
User types "1" (Create Group) + "room1"
  ↓
Client builds TLV:
  [0x07][0x00 0x0A]                    ← CREATE_GROUP_REQ
    [0x22][0x00 0x05][room1]           ← GROUP_NAME
  ↓
Server: handle_create_group()
  → GroupMng_CreateGroup("room1", client_id, mc_ip_out, mc_port_out)
    → HashMap_Find(group_hash, "room1") → not found
    → QueueRemove(free_mc_ip_queue) → "239.0.0.1"
    → calloc Group, strncpy name, mc_ip
    → group->mc_port = 5000
    → group->member_count = 1
    → ListCreate() for members, ListPushHead(int* client_id)
    → HashMap_Insert(group_hash, group->group_name, group)
    → fill out_mc_ip="239.0.0.1", out_mc_port=5000
  → send_group_resp():
      [0x08][len]
        [0x25][1][0x00]               ← STATUS=SUCCESS
        [0x23][len]["239.0.0.1"]      ← MC_IP
        [0x24][2][0x13 0x88]          ← MC_PORT=5000 in network byte order
  ↓
Client on SUCCESS:
  → extract mc_ip="239.0.0.1", mc_port=5000
  → ClientGroupsMng_Add(groups, "room1", "239.0.0.1", 5000)
    → strdup("room1") as key
    → malloc GroupEntry, fill fields, sender_pid=0, receiver_pid=0
    → HashMap_Insert
  → launch_chat_windows(mng, "room1", "239.0.0.1", 5000)
```

### 6.5 Window Launch Flow (Phase 4)

```
launch_chat_windows():

1. mq_open("/lanchat_rpid", O_CREAT|O_RDONLY, 0600, &attr)  ← receiver queue
   mq_open("/lanchat_spid", O_CREAT|O_RDONLY, 0600, &attr)  ← sender queue

2. system("gnome-terminal -- ./mc_receiver 239.0.0.1 5000")
   gnome-terminal forks and returns immediately
   mc_receiver starts in its own window:
     → mq_open("/lanchat_rpid", O_WRONLY)
     → mq_send(mq, &getpid(), sizeof(pid_t), 0)
     → socket(), SO_REUSEADDR, bind(INADDR_ANY:5000)
     → IP_ADD_MEMBERSHIP for 239.0.0.1
     → recvfrom() loop ...

3. system("gnome-terminal -- ./mc_sender 239.0.0.1 5000 alice")
   mc_sender starts in its own window:
     → mq_open("/lanchat_spid", O_WRONLY)
     → mq_send(mq, &getpid(), sizeof(pid_t), 0)
     → socket(), IP_MULTICAST_TTL=1
     → fgets() loop ...

4. mq_receive(mq_r, &receiver_pid, sizeof(pid_t), NULL)   ← blocks until mc_receiver sends PID
   mq_receive(mq_s, &sender_pid,   sizeof(pid_t), NULL)   ← blocks until mc_sender sends PID

5. GroupEntry *entry = ClientGroupsMng_Find(groups, "room1")
   entry->receiver_pid = receiver_pid
   entry->sender_pid   = sender_pid

6. mq_close + mq_unlink both queues
```

**Why two separate queues?** If both processes used one queue, they race to send first. The first PID to arrive is in the queue but we don't know if it's the sender or the receiver — we'd need extra metadata. Two dedicated queues remove all ambiguity: receiver PID always goes to `/lanchat_rpid`, sender PID always goes to `/lanchat_spid`.

### 6.6 Chat Message Flow (Server Not Involved)

```
User types "hello everyone" in the mc_sender window
  ↓
fgets() returns "hello everyone\n"
strip newline → "hello everyone"
snprintf → "alice: hello everyone"
sendto(sock, "alice: hello everyone", 21, 0, &dest_addr, sizeof(dest_addr))
  ↓
UDP packet sent to 239.0.0.1:5000

[LAN multicast routing]
  ↓
Every host that joined multicast group 239.0.0.1 receives the packet

Client B's mc_receiver:
  recvfrom() returns 21 bytes
  buf[21] = '\0'
  printf("alice: hello everyone\n")
  fflush(stdout)
```

**The server never touches this data.** Chat is fully peer-to-peer via UDP multicast.

### 6.7 Leave Group Flow

```
User types "3" (Leave Group) + "room1"
  ↓
Client → [0x0B][len][0x22][len]["room1"]  → Server

Server: handle_leave_group()
  → GroupMng_Leave("room1", client_id)
    → HashMap_Find(group_hash, "room1") → Group found
    → remove_client_from_group(group, client_id)
      → Walk members list, find int* whose *value == client_id
      → free(id); ListItrRemove(itr); group->member_count--
      → If member_count == 0:
          strdup(group->mc_ip)
          HashMap_Remove(group_hash, group->group_name)
          ListDestroy(group->members, free)
          free(group)
          QueueInsert(free_mc_ip_queue, ip_copy)
  → STATUS_SUCCESS

Client on SUCCESS:
  → ClientGroupsMng_Remove(groups, "room1")
    → HashMap_Remove(map, "room1") → gets back key and entry
    → kill(entry->sender_pid, SIGTERM)    ← closes sender window
    → kill(entry->receiver_pid, SIGTERM)  ← closes receiver window
    → free(key); free(entry)

mc_sender on SIGTERM:
  → sigterm_handler fires: running = 0
  → fgets() returns NULL (stdin interrupted)
  → while loop exits
  → close(sock)
  → exit

mc_receiver on SIGTERM:
  → sigterm_handler fires: running = 0
  → recvfrom() returns -1 with errno == EINTR
  → loop checks: errno == EINTR → break
  → IP_DROP_MEMBERSHIP
  → close(sock)
  → exit
```

---

## 7. UDP Multicast — How Chat Actually Works

### 7.1 What is Multicast?

Normal UDP is **unicast** — one sender, one receiver. To reach 10 people you'd call `sendto()` 10 times. **Multicast** lets one `sendto()` call reach all subscribers simultaneously. The network hardware handles the fan-out.

Multicast addresses are in the range `224.0.0.0` to `239.255.255.255`. This project uses `239.0.0.x` — the **administratively scoped** range, which is guaranteed never to be routed beyond your LAN.

### 7.2 How a Receiver Joins a Group

```c
/* Create UDP socket */
int sock = socket(AF_INET, SOCK_DGRAM, 0);

/* Allow multiple receivers on the same machine and same port */
int reuse = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

/* Bind to the port on ALL interfaces (not to the multicast IP itself) */
struct sockaddr_in local;
local.sin_family      = AF_INET;
local.sin_port        = htons(5000);
local.sin_addr.s_addr = INADDR_ANY;   /* all local interfaces */
bind(sock, &local, sizeof(local));

/* Join the multicast group — tells kernel to deliver 239.0.0.5 packets here */
struct ip_mreq mreq;
inet_pton(AF_INET, "239.0.0.5", &mreq.imr_multiaddr);
mreq.imr_interface.s_addr = INADDR_ANY;
setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
```

**Why bind to `INADDR_ANY` and not to the multicast IP?**
Binding to the multicast IP itself is implementation-defined and unreliable across Linux versions. The correct, portable pattern is to bind to `INADDR_ANY` (which means "any local interface") and use `IP_ADD_MEMBERSHIP` to tell the kernel which multicast group to subscribe to. The kernel then filters incoming UDP packets and only delivers those addressed to `239.0.0.5` to this socket.

**Why `SO_REUSEADDR`?**
Without it, only one socket on the entire machine can bind to port 5000. If two people on the same laptop join the same group, the second receiver's `bind()` would fail with "Address already in use". `SO_REUSEADDR` lifts this restriction for multicast sockets.

### 7.3 How a Sender Works

```c
int sock = socket(AF_INET, SOCK_DGRAM, 0);

/* Set TTL to 1 — packet dies at the first router, stays on LAN */
unsigned char ttl = 1;
setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

/* No bind() needed — the OS picks the outgoing interface */

struct sockaddr_in dest;
dest.sin_family = AF_INET;
dest.sin_port   = htons(5000);
inet_pton(AF_INET, "239.0.0.5", &dest.sin_addr);

/* One sendto() reaches ALL receivers in the group */
sendto(sock, message, strlen(message), 0, &dest, sizeof(dest));
```

**Why TTL = 1?** TTL (Time To Live) is a hop counter. Every router that forwards a packet decrements it. At TTL = 0, the packet is dropped. Setting TTL = 1 means the packet is dropped at the first router — it cannot leave the LAN. This is the correct setting for a local chat application and prevents multicast traffic from leaking onto the internet.

### 7.4 SIGTERM and Signal Safety

When the client calls `kill(pid, SIGTERM)`, the OS delivers SIGTERM to the target process. The process must handle this to exit cleanly.

```c
static volatile sig_atomic_t running = 1;

static void sigterm_handler(int sig) {
    (void)sig;
    running = 0;   /* signal-safe write */
}

signal(SIGTERM, sigterm_handler);
```

**Why `volatile sig_atomic_t` and not `int`?**
- **`volatile`** tells the compiler not to cache this variable in a register. Without it, the compiler might optimize `while(running)` into `while(1)` because it doesn't see any code in the main loop that changes `running`.
- **`sig_atomic_t`** is the only integer type guaranteed by the C standard to be readable and writable atomically from signal handlers without a data race on all platforms.

**The two exit paths in mc_receiver:**

The receiver is blocked in `recvfrom()` when SIGTERM arrives. The OS interrupts the system call and `recvfrom()` returns `-1` with `errno == EINTR`. The loop checks:
```c
if (errno == EINTR) break;   /* signal interrupt — normal SIGTERM exit */
```

Two conditions are needed because:
- `errno == EINTR` catches the current blocked `recvfrom()` being interrupted.
- `running == 0` (checked at `while (running)`) catches future iterations after the signal.

---

## 8. Key Concepts Explained

### 8.1 The libds.so Library

This project uses a pre-built data structures library at `~/Documents/Embedded_and_RTOS/DS/`. It provides:

| Module | Used By | Purpose |
|--------|---------|---------|
| `HashMap` | UserMng, GroupMng, ClientGroupsMng | O(1) key-value lookup |
| `LinkedList` (doubly) | ServerNet, GroupMng (members) | O(1) insert/remove, sequential scan |
| `GenQueue` | GroupMng | FIFO for multicast IP pool |

**HashMap internals (from API usage):**
- `HashMap_Create(capacity, HashFunc, EqualFunc)` — creates a hash table with an initial bucket count.
- `HashMap_Insert(map, key, value)` — stores a key-value pair. Does NOT copy key or value — stores the pointers as-is.
- `HashMap_Find(map, key, &value_out)` — computes hash, scans bucket, uses EqualFunc (strcmp) to find match.
- `HashMap_Remove(map, key, &key_out, &value_out)` — removes entry, returns BOTH pointers so caller can free them.
- `HashMap_ForEach(map, callback, context)` — calls `callback(key, value, context)` for every entry. Return 0 from callback to stop early.
- `HashMap_Destroy(&map, key_destructor, value_destructor)` — frees all internals. If destructors are NULL, does not free keys/values (caller's responsibility).

**Critical rule:** HashMap stores raw pointers. It is the caller's responsibility to ensure keys outlive their entries. This is why `strdup` is used for string keys — the heap copy persists independently.

### 8.2 POSIX Message Queues

POSIX message queues (`mqueue.h`) allow unrelated processes to exchange fixed-size messages through a kernel-maintained queue, identified by a name starting with `/`.

```
mq_open("/lanchat_rpid", O_CREAT|O_RDONLY, 0600, &attr)
  → Creates the queue in the kernel. Only client_mng reads from it.

mc_receiver:
  mq_open("/lanchat_rpid", O_WRONLY)   → opens the already-created queue for writing
  mq_send(mq, &my_pid, sizeof(pid_t), 0)  → sends the PID

client_mng:
  mq_receive(mq_r, &pid, sizeof(pid_t), NULL)  → blocks until message arrives

mq_close() + mq_unlink()  → destroy the kernel queue after use
```

**Why `mq_unlink` at the end?** POSIX message queues persist in the kernel until explicitly deleted — even after all processes close their descriptors. Without `mq_unlink`, the queue would remain in the kernel across multiple join operations and old PIDs could be read by accident.

### 8.3 The `select()` vs Threads Tradeoff

The server handles multiple clients with a single thread using `select()`. An alternative would be to spawn a new thread per client.

| Approach | Pros | Cons |
|----------|------|------|
| `select()` single thread | No race conditions, no mutex, simple memory model | Must rebuild fd_set each iteration, limited by FD_SETSIZE (typically 1024 fds) |
| Thread per client | Each client handled independently, blocking I/O is fine | Need mutex on shared data (user/group maps), risk of deadlock, higher memory usage |

For a LAN chat application with tens of clients, `select()` is perfectly appropriate and much simpler to reason about.

---

## 9. How Bad Code Breaks Things

### 9.1 Missing Null-Terminator on TLV Strings

TLV values are **NOT null-terminated**. A USERNAME TLV for "alice" contains exactly 5 bytes: `61 6C 69 63 65`. There is no `\0` at byte 5.

**Bad code:**
```c
const uint8_t *username_val;
uint16_t username_len;
tlv_find_field(val, val_len, TAG_USERNAME, &username_val, &username_len);

/* WRONG — treating raw TLV bytes as a C string */
strcmp(username_val, "alice");    /* reads past the 5 bytes until it hits a \0 — UB */
UserMng_Register((char *)username_val, ...);   /* same problem */
```

**Correct code:**
```c
char uname[MAX_NAME_LEN] = {0};
memcpy(uname, username_val, username_len);   /* copy exactly username_len bytes */
/* uname[username_len] is already '\0' because of the = {0} initializer */
strcmp(uname, "alice");   /* safe */
```

This is why every handler in `server_mng.c` uses `char uname[MAX_NAME_LEN] = {0}` + `memcpy` before using the string.

### 9.2 Single `send()` Call (Partial Write Bug)

**Bad code (server's current bug):**
```c
int sent = send(node->socket_fd, msg, msg_len, 0);
return (sent == msg_len) ? 0 : -1;
```

**Scenario that breaks it:**
- Message is 50 bytes.
- Kernel send buffer has space for only 30 bytes.
- `send()` returns 30. The remaining 20 bytes are silently dropped.
- Client receives 30 bytes: a complete TLV header + part of the value.
- Client's RecvMsg reassembles: "I need 50 bytes total, I have 30, waiting..."
- Server thinks it succeeded (returns 0 to caller).
- No more bytes ever come for this message.
- Client waits forever — deadlock.

**Correct code (write loop):**
```c
int sent = 0;
while (sent < msg_len) {
    ssize_t n = send(sock, msg + sent, msg_len - sent, 0);
    if (n <= 0) return -1;
    sent += n;
}
```

### 9.3 Forgetting `ntohs()` on the Length Field

**Bad code:**
```c
uint16_t msg_len;
memcpy(&msg_len, recv_buf + 1, 2);
/* No ntohs()! */
int total = 3 + msg_len;
```

**What happens on a little-endian machine (x86):**
- Server sends length = 20 as bytes `[0x00][0x14]` (big-endian: 0x00 * 256 + 0x14 = 20).
- `memcpy` puts `0x14` in the low byte of `msg_len`, `0x00` in the high byte.
- Little-endian interpretation: `0x0014` = 20. Happens to be correct in this case.

But for length = 300 (`[0x01][0x2C]` in big-endian):
- `memcpy` produces: low byte = `0x2C`, high byte = `0x01`.
- Little-endian: `0x012C` = 300. Still correct!

Actually wait — little-endian places low byte first in memory. `memcpy` copies bytes as-is. If the first byte in the buffer is the high byte (big-endian wire format), `memcpy` into a `uint16_t` on little-endian gives:

```
Buffer: [0x01][0x2C]
memcpy into uint16_t:
  addr+0 → low byte of uint16_t = 0x01
  addr+1 → high byte of uint16_t = 0x2C
uint16_t value = 0x2C01 = 11265 (WRONG! Expected 300)
```

So the receiver would wait for 11268 bytes that never come. The program deadlocks until the connection is dropped by timeout.

### 9.4 Modifying a HashMap During ForEach

**Bad code (current bug #1):**
```c
/* Inside a HashMap_ForEach callback: */
static int remove_client_from_group_cb(const void *key, void *value, void *context)
{
    remove_client_from_group((Group *)value, *(int *)context);
    return 1;
}

/* Inside remove_client_from_group, when the group becomes empty: */
HashMap_Remove(s_group_hash, group->group_name, &key, &removed);  /* CRASH */
```

**What happens internally:**
The HashMap is a bucket array. `ForEach` iterates bucket by bucket, entry by entry. When `HashMap_Remove` is called, it finds the entry in its bucket and removes it — this modifies the bucket's linked list or open-addressing probe sequence. ForEach's internal cursor is now pointing at either:
- A freed node (use-after-free crash on next iteration).
- The wrong next entry (skips a group, leaving a client in it).

**Correct approach:**
```c
/* Phase 1: ForEach — just adjust counters, collect groups to destroy */
static int mark_for_removal_cb(const void *key, void *value, void *context)
{
    /* ... remove client from members list, decrement count ... */
    /* Do NOT call HashMap_Remove here */
    return 1;
}
HashMap_ForEach(s_group_hash, mark_for_removal_cb, &client_id);

/* Phase 2: separate pass — destroy empty groups */
/* (iterate over a collected list of empty group names and call HashMap_Remove) */
```

### 9.5 Stack Pointer as HashMap Key (Dangling Pointer)

**Bad code (current bug #2):**
```c
StatusCode UserMng_Login(..., int client_id)
{
    char id_key[16];                          /* lives on THIS stack frame */
    snprintf(id_key, ..., "%d", client_id);
    HashMap_Insert(hash, id_key, user);       /* stores &id_key[0] */
}   /* ← stack frame destroyed here, id_key is gone */

/* Later: */
User *UserMng_GetByClientId(int client_id)
{
    char search_key[16];
    snprintf(search_key, ..., "%d", client_id);
    HashMap_Find(hash, search_key, &val);
    /* HashMap calls strcmp(search_key, stored_key) */
    /* stored_key is id_key from the old stack frame — garbage! */
}
```

**What happens in memory:**

```
Time 1: UserMng_Login() stack frame at address 0x7fff1000
  id_key[16] at 0x7fff1000 = "3\0"
  HashMap stores pointer 0x7fff1000

Time 2: UserMng_Login() returns — 0x7fff1000 is now free stack space

Time 3: Some other function uses the stack
  Its local variables overwrite 0x7fff1000 with e.g. 0x00 (zero)

Time 4: HashMap_Find compares:
  strcmp(search_key="3", stored_key=0x7fff1000="")
  → empty string, doesn't match "3"
  → user not found → wrong behavior
```

**Correct code:**
```c
char *key = strdup(id_key);   /* heap copy — persists until explicitly freed */
HashMap_Insert(hash, key, user);

/* On logout: */
void *key_out, *val_out;
HashMap_Remove(hash, id_key, &key_out, &val_out);
free(key_out);   /* now safely free the heap copy */
```

---

## 10. Best Practices Review

### What the Code Does Right

| Practice | Where |
|----------|-------|
| Callbacks for decoupling | `ServerNet` uses `OnMessageCb` / `OnDisconnectCb` — Net layer knows nothing about management logic |
| Opaque structs | `ClientGroupsMng` hidden struct forces all access through official API |
| `strdup` for HashMap keys (client) | `ClientGroupsMng_Add` correctly uses `strdup` + frees both key and value on remove |
| `volatile sig_atomic_t` for signal flags | `mc_sender` and `mc_receiver` correctly use signal-safe types |
| Write loop for send | `ClientNet_SendMsg` correctly retries partial writes |
| TLV reassembly buffer | Both client and server correctly handle partial TLV arrivals |
| `htons` / `ntohs` consistently | Used everywhere byte order conversion is needed |
| `memmove` not `memcpy` for buffer shifts | Correctly handles overlapping regions when consuming TLVs from the buffer |
| Pre-advance iterator before remove | `process_all_clients` saves `itr = ListItrNext(itr)` BEFORE calling `ListItrRemove` |
| Two queues for PIDs | `/lanchat_rpid` and `/lanchat_spid` eliminate the PID identification race condition |
| `memset(&addr, 0, sizeof(addr))` | Correctly zeroes struct before filling, avoids garbage in padding fields |
| `fgets` + `strcspn` for string input | Safe against spaces in usernames, correctly strips newline |

### What the Code Gets Wrong (Summary of Bugs)

| Practice Violated | Bug | Risk |
|-------------------|-----|------|
| Never modify a container while iterating it | Bug 1 — ForEach + Remove | Crash on disconnect |
| All keys stored in HashMap must outlive their entries | Bug 2 — stack key | Undefined behavior |
| Always retry partial `send()` | Bug 3 — single send call | Protocol desync |
| Handle `EINTR` from blocking syscalls | Bugs 4 + 11 — select/recv EINTR | Server crash on any signal |
| Always null-check `malloc` | Bugs 5 + 8 — unchecked allocs | Null dereference crash |
| Validate preconditions before modifying state | Bug 6 — no duplicate join check | Corrupted member count |
| Return meaningful error codes | Bug 7 — always SUCCESS from Leave | Silent wrong behavior |
| Initialize all fields of allocated structs | Bug 9 — username uninitialized | Latent garbage data |
| Blocking I/O needs a timeout | Bug 10 — infinite mq_receive | Client hangs forever |

### General C Programming Rules Illustrated by This Project

1. **Network byte order is mandatory.** Every 2+ byte value crossing a network boundary must go through `htons()`/`ntohs()`. Forgetting it causes protocol breakage that only appears on mixed-endian systems or for values > 127.

2. **TCP is not a message protocol.** Always buffer incoming data and wait for a complete application-layer message before processing. Never assume one `recv()` = one message.

3. **A single `send()` is not reliable.** Always use a write loop that advances the pointer and tracks remaining bytes. This is one of the most common bugs in network C code.

4. **Stack memory disappears when the function returns.** Never give a pointer to stack memory to any structure that outlives the function — hash maps, linked lists, queues, global variables. Use `strdup()` / `malloc()` to make heap copies.

5. **Signal handlers are asynchronous.** They can fire between any two instructions of your main code. Only `volatile sig_atomic_t` is safe to read/write from a signal handler. All other operations (heap allocation, printf, most library functions) are unsafe.

6. **`EINTR` is not an error.** Any blocking syscall (`select`, `recv`, `read`, `accept`, `mq_receive`) can be interrupted by a signal and return -1 with `errno == EINTR`. This means "try again" — not "something broke."

7. **Never modify a data structure while iterating it.** Hash maps, linked lists, and trees maintain internal cursor state during iteration. Inserting or removing during iteration corrupts that state. Collect items to modify, then modify them after the iteration completes.

8. **Free everything on every code path.** In C, there is no garbage collector. Every `malloc`/`strdup`/`calloc` must have a matching `free` on every exit path — including error paths in the middle of a function. Use `goto cleanup` patterns for complex resource management.
