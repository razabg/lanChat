# LAN Chat — Session Notes

---

## How TLV works

Every piece of data is wrapped in a 3-part envelope:

```
┌─────────┬──────────────┬─────────────────────┐
│ Tag     │ Length       │ Value               │
│ 1 byte  │ 2 bytes      │ `Length` bytes      │
└─────────┴──────────────┴─────────────────────┘
```

- **Tag** — what this data is (e.g. `0x20` = username)
- **Length** — how many bytes follow in the value
- **Value** — the actual payload bytes

### Example: REGISTER_REQ ("alice" / "1234")

**Username TLV:**
```
Tag    = 0x20           (USERNAME)
Length = 0x00 0x05      (5 bytes)
Value  = 61 6C 69 63 65 ("alice")
Raw:  20 00 05 61 6C 69 63 65
```

**Password TLV:**
```
Tag    = 0x21           (PASSWORD)
Length = 0x00 0x04      (4 bytes)
Value  = 31 32 33 34    ("1234")
Raw:  21 00 04 31 32 33 34
```

**Outer REGISTER_REQ TLV (wraps both):**
```
Tag    = 0x01           (REGISTER_REQ)
Length = 0x00 0x0F      (15 bytes = 8 + 7)
Value  = [ username TLV ] [ password TLV ]
```

**Full wire bytes:**
```
01  00 0F  20  00 05  61 6C 69 63 65  21  00 04  31 32 33 34
│   │      │   │      │               │   │      │
│   │      │   │      "alice"         │   │      "1234"
│   │      │   └── length=5          │   └── length=4
│   │      └── tag=USERNAME(0x20)    └── tag=PASSWORD(0x21)
│   └── length=15
└── tag=REGISTER_REQ(0x01)
```

---

## Alternative to nested TLV

Instead of nesting, you can flatten all TLVs in sequence with a length prefix:

```
[ msg_tag 1B ] [ total_len 2B ] [ field TLVs... ]
```

Without the outer length you need another way to know where the message ends:

| Approach | How you know it's done |
|---|---|
| Nested TLV | Outer length field — read exactly N bytes |
| Fixed field count | REGISTER always has exactly 2 fields |
| Delimiter | Special end-of-message byte (e.g. `0xFF`) |
| Length prefix | Prepend total message length as 2 bytes |

Nested TLV is cleanest for TCP streams — you always know exactly how many bytes to read.

---

## Message Queue — PID handshake

### What is a POSIX message queue?

An OS-level mailbox that lets separate processes send discrete messages to each other. Identified by a name string like `"/lanchat_pids"`. Any process that knows the name can read or write.

```c
#include <mqueue.h>

mqd_t mq = mq_open("/lanchat_pids", O_CREAT | O_RDWR, 0644, NULL);
mq_send(mq, (char*)&my_pid, sizeof(pid_t), 0);
mq_receive(mq, (char*)&pid, sizeof(pid_t), NULL);
mq_close(mq);
mq_unlink("/lanchat_pids");
```

### The MQ does NOT route to a specific process

It's a shared mailbox — whoever opens the same name shares the same queue. Client Mng just reads whatever arrives.

### Communication map

| Communication | Transport |
|---|---|
| mc_sender → multicast group | UDP socket |
| mc_receiver ← multicast group | UDP socket |
| mc_sender PID → Client Mng | Message queue |
| mc_receiver PID → Client Mng | Message queue |
| Client Mng → kill mc_sender | `kill(pid, SIGTERM)` |
| Client Mng → kill mc_receiver | `kill(pid, SIGTERM)` |

### Multi-group problem

If two groups are joined simultaneously, 4 child processes all send PIDs to the same queue. Solution: **sequential launch** — launch one child, read its PID immediately, then launch the next.

---

## system() and gnome-terminal

`system()` internally calls fork + exec but does NOT return the child PID.

```
Client Mng
    │
    ├── fork() ──► child (sh)
    │                   └── exec gnome-terminal
    │                               └── gnome-terminal forks again ──► mc_sender
    └── waitpid(sh) ──► returns (not mc_sender's PID)
```

That's why the message queue is needed — two forks deep, no handle to the final process.

### Usage

```c
char cmd[256];

// receiver window
snprintf(cmd, sizeof(cmd),
    "gnome-terminal -- ./mc_receiver %s %d",
    mc_ip, mc_port);
system(cmd);

// sender window
snprintf(cmd, sizeof(cmd),
    "gnome-terminal -- ./mc_sender %s %d %s",
    mc_ip, mc_port, username);
system(cmd);
```

Note: `./mc_sender` requires the binary to be in the same directory the client was launched from. Use absolute paths to be safe.

---

## Iterate all vs Destroy all (Client Groups Mng)

| | Iterate all | Destroy all |
|---|---|---|
| When | Logout | Full client shutdown |
| After | List is empty but still exists (ready for reuse) | List structure itself is freed and gone |

```
logout   → kill all PIDs + remove all entries  (list stays alive, empty)
shutdown → kill all PIDs + remove all entries + free the list itself
```

---

## Why IP_ADD_MEMBERSHIP is only on the receiver

Joining a multicast group means "I want to receive packets sent to this address" — it's a receive-side concept.

Without it, the OS ignores packets sent to `239.0.0.x` (not your machine's IP).
`IP_ADD_MEMBERSHIP` tells the OS to also accept and deliver those packets.

```c
// sender — no join needed, just sendto()
sendto(sock, message, len, 0, (struct sockaddr*)&multicast_addr, sizeof(multicast_addr));

// receiver — must join first
struct ip_mreq mreq;
mreq.imr_multiaddr.s_addr = inet_addr("239.0.0.1");
mreq.imr_interface.s_addr = INADDR_ANY;
setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
```

**Analogy:** Multicast is like a radio frequency. The sender just broadcasts — no registration. The receiver must tune in (`IP_ADD_MEMBERSHIP`) to hear it.
