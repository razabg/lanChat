# Session Log — Client-Side Implementation

**Project:** LAN Chat (Embedded & RTOS)
**Date:** 2026-05-30
**Scope:** Full client-side implementation session — `client_net`, `ui`, `client_mng`

---

## Overview of What Was Built

| File | Status | Notes |
|------|--------|-------|
| `common/protocol.c` | Reviewed — complete | No changes needed |
| `client/client_net.c` | Implemented | 4 phases |
| `client/ui.c` | Reviewed + patched | Added `showMessage`, fixed invalid-input enum values |
| `client/ui.h` | Patched | Added `MENU_INVALID`, `GROUP_MENU_INVALID`, `showMessage` |
| `client/test_ui.c` | Updated | Fixed fd leak, updated to use named enum constants |
| `client/client_mng.h` | Implemented | Struct + 3 public API declarations |
| `client/client_mng.c` | Implemented | 4 phases, all 6 handlers |

---

## Part 1 — `client_net.c`

### Purpose
The wire layer. Owns the TCP socket and the partial-TLV receive buffer. Every byte sent to or received from the server passes through here.

### Phase 1 — `ClientNet_Create`

Opens a TCP socket and connects to the server.

**Key calls:**
- `socket(AF_INET, SOCK_STREAM, 0)` — requests a TCP file descriptor from the OS
- `inet_pton()` — converts human-readable IP string to 4-byte binary; also validates the string
- `htons(port)` — converts port to network byte order (big-endian), required by socket API
- `connect()` — performs the TCP three-way handshake

Returns `NULL` on failure (never calls `exit`) — lets the caller decide.

```c
ClientNet *ClientNet_Create(const char *server_ip, uint16_t port)
{
    ClientNet *net = malloc(sizeof(ClientNet));
    if (!net) { perror("ClientNet_Create: malloc"); return NULL; }

    net->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (net->sockfd < 0) { perror("ClientNet_Create: socket"); free(net); return NULL; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        close(net->sockfd); free(net); return NULL;
    }
    if (connect(net->sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(net->sockfd); free(net); return NULL;
    }

    net->buf_len = 0;
    return net;
}
```

---

### Phase 2 — `ClientNet_SendMsg`

Sends all bytes of a TLV buffer reliably.

**Why a loop?** `send()` may transmit fewer bytes than requested when the kernel send buffer is full. A single call would silently truncate the message and the server would receive malformed TLV data. The loop keeps calling `send(buf + sent, remaining)` until every byte is delivered.

```c
int ClientNet_SendMsg(ClientNet *net, const uint8_t *buf, uint16_t len)
{
    uint16_t sent = 0;
    while (sent < len) {
        ssize_t n = send(net->sockfd, buf + sent, len - sent, 0);
        if (n <= 0) { perror("ClientNet_SendMsg: send"); return -1; }
        sent += (uint16_t)n;
    }
    return 0;
}
```

---

### Phase 3 — `ClientNet_RecvMsg`

The most important function. Reads raw bytes from the socket, accumulates them in an internal buffer, and returns exactly one complete TLV when enough bytes have arrived.

**Why a buffer?** TCP is a byte stream — no message boundaries. A 20-byte TLV might arrive in two `recv()` calls of 8 + 12 bytes, or it might arrive with the first 3 bytes of the next message. We must buffer and wait.

**TLV wire layout:**
```
[ tag: 1 byte ][ length: 2 bytes, big-endian ][ value: length bytes ]
```

**5-step algorithm:**

| Step | What happens |
|------|-------------|
| A | `recv()` appends new bytes to the tail of `recv_buf` |
| B | Check if we have at least 3 bytes (the header) |
| C | Read the 2-byte length field with `ntohs()` |
| D | Check if we have all `3 + length` bytes |
| E | Copy complete TLV to `out_buf`, `memmove` buffer left to consume it |

**Critical implementation note — `ntohs` vs manual:**
```c
uint16_t value_len;
memcpy(&value_len, net->recv_buf + 1, sizeof(uint16_t));
value_len = ntohs(value_len);  // big-endian network → host byte order
```

Returns: total byte count of TLV on success, `0` if not ready yet, `-1` on error.

---

### Phase 4 — `ClientNet_Destroy`

`close(sockfd)` sends TCP FIN to server (graceful shutdown). `free(net)` releases the struct. NULL-safe.

---

## Part 2 — `ui.c` / `ui.h`

### What already existed
- `showMenu()` — prints Screen 1 (Register / Login / Exit)
- `getMenuChoice()` — reads and returns a `MenuOption`
- `getCredentials()` — reads username + password into a `Credentials` struct
- `showGroupMenu()` — prints Screen 2 (Create / Join / Leave / Logout)
- `getGroupMenuChoice()` — reads and returns a `GroupMenuOption`
- `getGroupName()` — reads a group name string from stdin

### Changes Made

**1 — Added `MENU_INVALID = 0` and `GROUP_MENU_INVALID = 0` to the enums**

Before: returning `0` on bad input was a magic number. After: it's a named enum value so `client_mng` can write `if (choice == MENU_INVALID)` instead of `if (choice == 0)`.

```c
typedef enum {
    MENU_INVALID  = 0,   /* bad / non-numeric input */
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

**2 — Added `showMessage(const char *msg)`**

Without this, `client_mng` would call `printf` directly for server feedback, breaking the thin-layer principle. All terminal output goes through `ui.c`.

```c
void showMessage(const char *msg)
{
    printf("\n>> %s\n", msg);
}
```

### Test Results (`test_ui.c`)

All 16 tests passed:

```
[PASS] showMessage runs without crash
[PASS] showMenu runs without crash
[PASS] showGroupMenu runs without crash
[PASS] choice 1 → MENU_REGISTER
[PASS] choice 2 → MENU_LOGIN
[PASS] choice 3 → MENU_EXIT
[PASS] non-numeric input → MENU_INVALID
[PASS] choice 1 → GROUP_MENU_CREATE
[PASS] choice 2 → GROUP_MENU_JOIN
[PASS] choice 3 → GROUP_MENU_LEAVE
[PASS] choice 4 → GROUP_MENU_LOGOUT
[PASS] non-numeric input → GROUP_MENU_INVALID
[PASS] username parsed correctly
[PASS] password parsed correctly
[PASS] username has no trailing newline
[PASS] password has no trailing newline
```

**`set_stdin` fix:** original leaked a `FILE*` on every call. Fixed by tracking the previous tmpfile with a `static FILE *prev` and closing it before opening the next one.

---

## Part 3 — `client_mng.h`

### The struct

```c
typedef struct {
    ClientNet   *net;     // the TCP connection — all sends/receives go through here
    ClientState  state;   // SCREEN_1 or SCREEN_2
    int          running; // loop control flag — set to 0 to exit Run()
} ClientMng;
```

**`state`** — flips between `SCREEN_1` and `SCREEN_2`. Login flips 1→2, Logout flips 2→1.

**`running`** — cleaner than `break` or `exit()`. When set to 0, the loop exits and `Destroy` still runs, freeing all memory.

### Public API

| Function | Description |
|----------|-------------|
| `ClientMng_Create(ip, port)` | malloc struct, call `ClientNet_Create`, set `state=SCREEN_1, running=1` |
| `ClientMng_Run(mng)` | main loop — blocks until user exits |
| `ClientMng_Destroy(mng)` | `ClientNet_Destroy` + `free`. NULL-safe |

---

## Part 4 — `client_mng.c`

### Dependency tree

```
client_mng  →  ui.h           (display + input)
            →  client_net.h   (send/recv bytes)
            →  protocol.h     (tlv_encode / tlv_decode / tlv_find_field)
            →  client_groups_mng.h  (Phase 3 — not yet wired)
```

---

### Phase 1 — Lifecycle skeleton

`ClientMng_Create` / `ClientMng_Destroy` / `ClientMng_Run` (with stubs).

The Run loop:
```c
while (mng->running)
{
    if (mng->state == SCREEN_1)
    {
        showMenu();
        switch (getMenuChoice()) {
            case MENU_REGISTER: handle_register(mng); break;
            case MENU_LOGIN:    handle_login(mng);    break;
            case MENU_EXIT:     mng->running = 0;     break;
            default:            showMessage("Invalid option, try again."); break;
        }
    }
    else
    {
        showGroupMenu();
        switch (getGroupMenuChoice()) {
            case GROUP_MENU_CREATE:  handle_create_group(mng); break;
            case GROUP_MENU_JOIN:    handle_join_group(mng);   break;
            case GROUP_MENU_LEAVE:   handle_leave_group(mng);  break;
            case GROUP_MENU_LOGOUT:  handle_logout(mng);       break;
            default:                 showMessage("Invalid option, try again."); break;
        }
    }
}
```

---

### Phase 2 — Helper functions + Screen 1 handlers

#### `send_and_recv` helper

Every handler needs: send request → wait for response. Centralised here.

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
    do { n = ClientNet_RecvMsg(mng->net, recv_buf, recv_size); } while (n == 0);
    if (n < 0) {
        showMessage("Error: lost connection to server.");
        mng->running = 0;
        return -1;
    }
    return n;
}
```

**Why `do { } while (n == 0)`?** `RecvMsg` returns `0` when bytes arrived but don't yet form a complete TLV. Since `recv()` is blocking, this tight loop is correct — it will get more data on the next call.

#### `status_to_message` helper

Maps every `StatusCode` to a human-readable string. Every handler ends with one line: `showMessage(status_to_message(status_val[0]))`.

#### `handle_register` and `handle_login`

The two-level TLV pattern:

```
inner buffer:  [ USERNAME TLV ][ PASSWORD TLV ]
                       ↓
outer buffer:  [ REGISTER_REQ ][ inner_len ][ inner buffer ]
```

`tlv_encode` is called 3 times. The first two write the field TLVs back-to-back into `inner[]`. The third wraps `inner[]` as the value of the outer message TLV.

`handle_login` is identical except it sets `mng->state = SCREEN_2` on `STATUS_SUCCESS`.

---

### Phase 3 — `handle_logout`

`LOGOUT_REQ` carries no payload — the server identifies the user by their TCP `client_id`. The message is 3 bytes:

```
[ 0x05 ][ 0x00 ][ 0x00 ]
  tag     length = 0
```

On `STATUS_SUCCESS`: `mng->state = SCREEN_1`.

```c
uint8_t msg[TLV_HEADER_SIZE];
int msg_len = tlv_encode(msg, sizeof(msg), TAG_LOGOUT_REQ, NULL, 0);
```

---

### Phase 4 — Group handlers

#### `handle_create_group` and `handle_join_group`

These are the first handlers that extract **more than just status** from the response. The response value block contains three fields:

```
[ TAG_STATUS  ][ 1  ][ code ]
[ TAG_MC_IP   ][ len ][ "239.0.0.x" ]   ← UTF-8, NOT null-terminated
[ TAG_MC_PORT ][ 2  ][ port big-endian ]
```

**Critical: MC_IP null-termination**
```c
/* WRONG — ip_val has no null terminator in the TLV */
printf("%s", ip_val);

/* CORRECT */
char mc_ip[64];
memcpy(mc_ip, ip_val, ip_len);
mc_ip[ip_len] = '\0';
```

**Critical: MC_PORT byte order**
```c
/* WRONG on little-endian x86 — bytes are reversed */
uint16_t mc_port;
memcpy(&mc_port, port_val, 2);

/* CORRECT — manually reconstruct big-endian to host */
uint16_t mc_port = ((uint16_t)port_val[0] << 8) | port_val[1];
```

Both functions have `TODO` markers where `client_groups_mng` (Phase 3) and window launching (Phase 4) will plug in.

#### `handle_leave_group`

Sends `LEAVE_GROUP_REQ` with the group name. Response contains only `TAG_STATUS`. Has a `TODO` for killing sender/receiver PIDs via `client_groups_mng` on success.

---

## What Remains (Client Side)

| Module | Phase | Status |
|--------|-------|--------|
| `client_groups_mng.c/.h` | Phase 3 | Not implemented |
| `client_main.c` | Phase 3 | Empty — 3 lines once mng is wired |
| `mc_sender.c` | Phase 4 | Empty |
| `mc_receiver.c` | Phase 4 | Empty |
| Wire `client_groups_mng` into `client_mng` | Phase 4 | TODO markers in place |
| Window launching via `gnome-terminal` + `system()` | Phase 4 | TODO markers in place |
| PID retrieval via message queue | Phase 4 | Not started |

---

## Compile Check (all files)

```bash
gcc -Wall -Wextra -g -I. -Iclient -Icommon -c client/client_mng.c  # OK
gcc -Wall -Wextra -g -I. -Iclient -Icommon -c client/client_net.c  # OK
gcc -Wall -Wextra -g -I. -Iclient -Icommon -c client/ui.c          # OK
gcc -Wall -Wextra -g -I. -Icommon          -c common/protocol.c    # OK
```

Zero warnings across all files.