# Multicast — Phase 4 Full Explanation

**Project:** LAN Chat (Embedded & RTOS)
**Date:** 2026-05-30

---

## 1. What is Multicast and Why We Use It

In a normal TCP connection, one sender talks to **one** receiver (unicast). If 5 clients are in a group and Alice sends a message, she would need 4 separate TCP sends — one to each person.

**Multicast solves this.** Alice sends **one** UDP packet to a special multicast IP address (e.g. `239.0.0.5`). The network delivers it to **every machine** that has joined that group address. One send, many receivers — the network does the fan-out.

```
Alice sends ONE packet to 239.0.0.5:5000
        │
        ├──▶ Bob's receiver   (joined 239.0.0.5)  prints it
        ├──▶ Carol's receiver (joined 239.0.0.5)  prints it
        └──▶ Alice's receiver (joined 239.0.0.5)  prints it too
```

The server is **not involved** in any of this. It only hands out the multicast IP + port when a group is created. After that, clients talk directly to each other via UDP.

---

## 2. Multicast IP Address Range

The reserved range for local multicast is `224.0.0.0 – 239.255.255.255`.

We use `239.0.0.1 – 239.0.0.254` — these are **administratively scoped** addresses. Routers will not forward them beyond the LAN. Perfect for a local chat application.

Each active group gets its own unique IP from this pool (managed by the server's Free MC IP Queue). Two different groups never share an IP, so their messages never mix.

---

## 3. The Two Standalone Programs

Phase 4 splits the chat window into **two separate executables** that run in their own terminals:

```
┌──────────────────────────┐    ┌───────────────────────────┐
│    mc_sender terminal     │    │   mc_receiver terminal     │
│                          │    │                            │
│  > hello everyone        │    │  alice: hello everyone     │
│  > how is everyone?      │    │  bob: doing great!         │
│                          │    │                            │
│  (user types here)       │    │  (messages appear here)    │
└──────────────────────────┘    └───────────────────────────┘
```

They are launched by `client_mng` via `gnome-terminal` + `system()` when a group is joined or created. Each pair of windows belongs to one group.

---

## 4. `mc_sender` — How it Works

**Launched with:**
```bash
./mc_sender <mc_ip> <mc_port> <username>
```

**Socket setup:**
```c
socket(AF_INET, SOCK_DGRAM, 0)       /* UDP socket — not TCP              */
setsockopt(IP_MULTICAST_TTL, 1)      /* TTL=1: stays on LAN, no routing   */
```

**No `bind()` on the sender** — we call `sendto()` directly targeting the multicast address. The OS picks the outgoing interface automatically.

**Main loop:**
```
read line from stdin
prepend username  →  "alice: hello everyone"
sendto(sock, message, len, 0, &multicast_addr, sizeof(multicast_addr))
repeat
```

**On SIGTERM:** a signal handler sets a `running = 0` flag. The loop checks the flag and exits cleanly after the current iteration.

---

## 5. `mc_receiver` — How it Works

**Launched with:**
```bash
./mc_receiver <mc_ip> <mc_port>
```

**Socket setup — more involved than the sender:**
```c
socket(AF_INET, SOCK_DGRAM, 0)

/* Allow multiple sockets on this machine to bind to the same port.
 * Without this, only one receiver per machine could join the group. */
setsockopt(SO_REUSEADDR, 1)

/* Bind to the port on ALL interfaces — not to the multicast IP.
 * The multicast membership (below) tells the kernel which group
 * to deliver packets from. */
bind(INADDR_ANY, mc_port)

/* Join the multicast group.
 * This tells the kernel: "deliver packets sent to mc_ip to my socket." */
struct ip_mreq mreq;
mreq.imr_multiaddr = mc_ip;       /* the multicast group address */
mreq.imr_interface.s_addr = INADDR_ANY;  /* any local interface  */
setsockopt(IP_ADD_MEMBERSHIP, &mreq)
```

**Why `bind(INADDR_ANY)` and not `bind(mc_ip)`?**
You bind to the port on all interfaces, then separately use `IP_ADD_MEMBERSHIP` to subscribe to the multicast group. This is the correct Linux multicast reception pattern — binding to the MC IP directly is unreliable across implementations.

**Main loop:**
```
recvfrom(sock, buffer, ...)   /* blocks until a packet arrives */
print buffer to stdout
repeat
```

**On SIGTERM — clean shutdown:**
```c
setsockopt(IP_DROP_MEMBERSHIP, &mreq)  /* leave the multicast group */
close(sock)
exit(0)
```

Dropping membership is important — it signals the network stack to stop delivering that group's packets to this socket.

---

## 6. PID Communication — The Message Queue

**The problem:** `client_mng` needs to store the PIDs of the sender and receiver processes so it can kill them later with `kill(pid, SIGTERM)`. But `system()` returns the exit code of the shell — not the PID of the launched program. And `gnome-terminal` itself forks another child, making PID tracking even harder.

**Solution: POSIX message queue.**

Both the sender and receiver call `getpid()` immediately on startup and send their PID to a named message queue. `client_mng` reads them back after launching both.

```
client_mng                         mc_sender / mc_receiver
─────────────────────────────────────────────────────────────
mq_open("/lanchat_pids", O_CREAT)

system("gnome-terminal -- ./mc_receiver ...")
                                    starts up
                                    getpid() → pid_r
                                    mq_send(pid_r)

system("gnome-terminal -- ./mc_sender ...")
                                    starts up
                                    getpid() → pid_s
                                    mq_send(pid_s)

mq_receive() → pid_r
mq_receive() → pid_s

GroupEntry *e = ClientGroupsMng_Find(mng->groups, group_name)
e->receiver_pid = pid_r
e->sender_pid   = pid_s

mq_close()
mq_unlink("/lanchat_pids")
```

The queue is identified by the **name** `"/lanchat_pids"` — a string both sides know in advance. It works like a pipe but between unrelated processes.

---

## 7. The Full Flow on Join / Create

```
handle_join_group or handle_create_group:

  Step 1 — Server responds with MC IP + Port
            e.g. "239.0.0.5", port 5000

  Step 2 — ClientGroupsMng_Add(group_name, "239.0.0.5", 5000)
            Entry stored in HashMap. PIDs = 0 for now.

  Step 3 — mq_open("/lanchat_pids", O_CREAT | O_RDWR)
            Message queue created.

  Step 4 — system("gnome-terminal -- ./mc_receiver 239.0.0.5 5000")
            New terminal opens.
            mc_receiver sends its PID to the queue.

  Step 5 — system("gnome-terminal -- ./mc_sender 239.0.0.5 5000 alice")
            New terminal opens.
            mc_sender sends its PID to the queue.

  Step 6 — mq_receive() → receiver_pid
            mq_receive() → sender_pid

  Step 7 — GroupEntry *e = ClientGroupsMng_Find(mng->groups, group_name)
            e->receiver_pid = receiver_pid
            e->sender_pid   = sender_pid

  Step 8 — mq_close() + mq_unlink("/lanchat_pids")
```

---

## 8. Closing the Windows — Leave / Logout

### On Leave

```
handle_leave_group  (on STATUS_SUCCESS from server):
    ClientGroupsMng_Remove(group_name)
        ├── kill(sender_pid,   SIGTERM)   → sender terminal closes
        ├── kill(receiver_pid, SIGTERM)   → receiver terminal closes
        ├── free(key)
        └── free(entry)
```

### On Logout

```
handle_logout  (on STATUS_SUCCESS from server):
    ClientGroupsMng_RemoveAll()
        ├── ForEach → kill all sender + receiver PIDs
        ├── HashMap_Destroy → free all keys and entries
        └── HashMap_Create  → fresh map for next session
    mng->state = SCREEN_1
```

---

## 9. Concept Reference Table

| Concept | API | Why |
|---------|-----|-----|
| UDP socket | `SOCK_DGRAM` | Multicast is connectionless — no handshake, no reliability overhead |
| Multicast TTL = 1 | `IP_MULTICAST_TTL` | Packets die at the first router — stay on the LAN |
| Join multicast group | `IP_ADD_MEMBERSHIP` | Kernel delivers packets for that MC address to our socket |
| Leave multicast group | `IP_DROP_MEMBERSHIP` | Clean exit — kernel stops delivering the group's packets |
| Multiple receivers on same port | `SO_REUSEADDR` | Multiple clients on the same machine can all bind port 5000 |
| PID retrieval | `mq_open / mq_send / mq_receive` | `system()` doesn't return child PIDs — message queue bridges the gap |
| Kill processes | `kill(pid, SIGTERM)` | Closes terminal windows when leaving a group or logging out |

---

## 10. Key Difference: TCP vs UDP in This App

| | TCP (management) | UDP Multicast (chat) |
|-|-----------------|----------------------|
| Used for | Register, login, logout, group ops | Actual chat messages |
| Direction | Client ↔ Server | Client ↔ All clients in group |
| Reliability | Guaranteed delivery, ordered | Best effort, no ordering |
| Server involved | Always | Never |
| Socket type | `SOCK_STREAM` | `SOCK_DGRAM` |
| Address | Server IP + port 9000 | Multicast IP + group port |