# mini_serv — deep explanation

---

## Big picture: what the server does

```
                        127.0.0.1 : port
                              |
                         [ sockfd ]   ← server socket, always listening
                        /     |     \
                   fd=4     fd=5    fd=6     ← client connections
                  client0  client1  client2
```

The server is a **single-process, single-thread chat relay**.  
It never blocks waiting on one client — it uses `select()` to watch all fds at once  
and only acts on fds that are ready.

---

## File descriptors (fd) — the foundation

In Linux **everything open is a number**:

```
fd 0  → stdin
fd 1  → stdout
fd 2  → stderr
fd 3  → sockfd (server socket, first thing we open)
fd 4  → first client that connects
fd 5  → second client
fd 6  → third client
...
```

`fd` is just an `int`. The kernel gives you the lowest available number.  
When you `close(fd)`, that number becomes free again and the next `accept()` may reuse it.

---

## The fd/id split — why two tracking arrays

The **fd** is the kernel's handle. It's not sequential per client:
- client 0 might be on fd 4, client 1 on fd 5, then client 1 disconnects
- next connect gets fd 5 again — but it should get id 2, not 1

So we need two separate things:

```
Global: int count = 0;       // next id to hand out, never goes down

Array:  ids[65536]           // ids[fd] = logical client id
                             // indexed by fd, size = max possible fd value

Array:  msgs[65536]          // msgs[fd] = heap buffer of partial message
                             // same indexing
```

```
After 3 connects, 1 disconnect, 1 reconnect:

  fd:    3      4       5      6
  ids:  [srv]  [0]    [2]    [1]    ← id 1 left, fd 5 reused, got id 2
  msgs: [NULL] [NULL] ["hel"] [NULL]
                               ^partial "hello\n" not yet complete
```

`ids[fd]` lets you look up "who is this fd" instantly when you need to format  
`"client %d: "` or `"server: client %d just left\n"`.

---

## struct sockaddr_in — the address struct

Defined in `<netinet/in.h>`. Describes an IPv4 address + port.

```c
struct sockaddr_in {
    sa_family_t    sin_family;   // always AF_INET (= IPv4)
    in_port_t      sin_port;     // port, must be in network byte order
    struct in_addr sin_addr;     // IP address
};

struct in_addr {
    uint32_t       s_addr;       // 32-bit IP as a single integer
};
```

**Usage in mini_serv:**
```c
servaddr.sin_family      = AF_INET;
servaddr.sin_addr.s_addr = htonl(2130706433);  // 127.0.0.1 as integer
servaddr.sin_port        = htons(atoi(argv[1]));
```

**Why `htonl` / `htons`?**  
Network protocol requires **big-endian** byte order. Your x86 CPU is little-endian.  
- `htonl` = host-to-network long  (32-bit, for the IP address)  
- `htons` = host-to-network short (16-bit, for the port)

`2130706433` in hex is `0x7F000001` = `127.0.0.1`  
(127 = 0x7F, 0, 0, 1)

You pass this struct to `bind()` which tells the kernel:  
*"attach this socket to this IP:port on this machine".*

---

## fd_set — how select() watches multiple fds

`fd_set` is defined in `<sys/select.h>` (pulled in via socket headers).  
Internally it is just a **bitfield** — one bit per possible fd number:

```
fd_set (visualised as bits, fd 0 on the right):

bit index:  ...  6  5  4  3  2  1  0
afds:            1  1  1  1  0  0  0
                 ^  ^  ^  ^
                 |  |  |  server socket (fd 3)
                 |  |  client 0 (fd 4)
                 |  client 1 (fd 5)
                 client 2 (fd 6)
```

**The four macros:**

| Macro | Effect |
|-------|--------|
| `FD_ZERO(&set)` | clear all bits → empty set |
| `FD_SET(fd, &set)` | set bit for fd → add to set |
| `FD_CLR(fd, &set)` | clear bit for fd → remove from set |
| `FD_ISSET(fd, &set)` | test bit → is fd in this set? |

---

## Three fd_sets and why

```c
fd_set rfds, wfds, afds;
```

| Name | Role |
|------|------|
| `afds` | **master set** — the ground truth of open fds. Never given to select(). |
| `rfds` | copy of afds given to select() as read-watch list. select() **overwrites** it with which fds have data ready to read. |
| `wfds` | copy of afds given to select() as write-watch list. select() **overwrites** it with which fds won't block on send(). |

**Why copy before every select()?**

```c
// EVERY iteration:
rfds = wfds = afds;   // restore from master — select() will clobber rfds and wfds
select(...);
// now rfds = "who sent data", wfds = "who can receive without blocking"
```

If you passed `afds` directly, select() would destroy it on the first call.

**Why wfds for send()?**

The subject says: *"client can be lazy and if they don't read your message you must NOT disconnect them"*.  
A lazy client's kernel receive buffer fills up. Calling `send()` on it would block your entire server.  
`select()` only sets that client's bit in `wfds` when its buffer has space.  
So sending only to `FD_ISSET(fd, &wfds)` = guaranteed non-blocking send.

---

## select() — the core

```c
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
```

- `nfds` = highest fd you care about + 1 (so kernel knows how far to scan the bitfield)
- Blocks until at least one fd in readfds or writefds is ready
- **Modifies** readfds and writefds in place to show only the ready ones
- Returns number of ready fds, or -1 on error

```
Before select():   rfds = { 3, 4, 5, 6 }   (all open)
                   wfds = { 3, 4, 5, 6 }

Client 4 sends data, client 6 is lazy:

After select():    rfds = { 4 }             (only fd 4 has data)
                   wfds = { 3, 4, 5 }       (fd 6 buffer full, not in wfds)
```

---

## Server lifecycle — step by step

```
1. socket()   → creates sockfd (fd 3), a bare endpoint, not yet bound to anything
                 FD_SET(sockfd, afds), max_fd = sockfd

2. bind()     → attaches sockfd to 127.0.0.1:port

3. listen()   → marks sockfd as passive — it will queue incoming connections

4. select()   → blocks until something happens

5a. sockfd readable?
      → new connection queued → accept() → returns client_fd (fd 4, 5, 6...)
      → register_client(client_fd)
      → break, restart select()

5b. client_fd readable?
      → recv() data into buf_read
      → if recv <= 0: client disconnected → remove_client()
      → else: append to msgs[fd] buffer, extract complete lines, forward

repeat 4-5 forever
```

---

## Function breakdown

### `register_client(fd)`

```c
max_fd = fd > max_fd ? fd : max_fd;   // keep max_fd current — select() needs it
ids[fd] = count++;                     // assign next sequential id
msgs[fd] = NULL;                       // no buffered data yet
FD_SET(fd, &afds);                     // add to master set
sprintf(buf_write, "server: client %d just arrived\n", ids[fd]);
notify_other(fd, buf_write);           // tell everyone else (not the new client)
```

The new client doesn't receive its own arrival message — `notify_other` skips the author.

---

### `remove_client(fd)`

```c
sprintf(buf_write, "server: client %d just left\n", ids[fd]);
notify_other(fd, buf_write);   // tell others before closing
free(msgs[fd]);                // release partial message buffer (free(NULL) is safe)
FD_CLR(fd, &afds);             // remove from master set
close(fd);                     // release the fd — kernel may reuse this number
```

Note: `ids[fd]` stays in the array with the old value. That's fine — the fd won't be  
read again until a new client takes it, at which point `register_client` overwrites it.

---

### `notify_other(author, str)`

```c
for (int i = 0; i <= max_fd; i++)
{
    if (FD_ISSET(i, &wfds) && i != author)
        send(i, str, strlen(str), 0);
}
```

Walks every fd up to max_fd.  
- `FD_ISSET(i, &wfds)` — only send to fds that select() said are ready to receive (non-blocking)
- `i != author` — don't echo back to the sender

Called multiple times for a single message (prefix + content) — that's intentional and fine.  
TCP streams them back-to-back; the receiver sees them as one continuous stream.

---

### `send_msg(fd)`

```c
while (extract_message(&msgs[fd], &msg))
{
    sprintf(buf_write, "client %d: ", ids[fd]);
    notify_other(fd, buf_write);   // prefix: "client 2: "
    notify_other(fd, msg);         // the line itself: "hello\n"
    free(msg);
}
```

The `while` loop is essential — a single `recv()` may contain **multiple complete lines**.  
`extract_message` peels them off one by one.

---

### `extract_message(buf, msg)` — how it works

```
State before:  *buf = "hello\nworld\nstill"
                       ^--- finds '\n' at i=5

Actions:
  newbuf = "world\nstill"   (everything after the first \n)
  *msg   = "hello\n"        (truncated at i+1, the old *buf pointer)
  *buf   = newbuf            (caller's msgs[fd] now points to remainder)

Returns 1.

Next call:
  *buf = "world\nstill"  → finds '\n' at i=5
  *msg = "world\n"
  *buf = "still"
  Returns 1.

Next call:
  *buf = "still"  → no '\n' found
  Returns 0.  → while loop exits, "still" stays buffered for next recv()
```

---

### `str_join(buf, add)` — growing the per-client buffer

```
msgs[fd] starts as NULL.

recv() → "hel"
  str_join(NULL, "hel")  → malloc("hel")  → msgs[fd] = "hel"

recv() → "lo\n"
  str_join("hel", "lo\n") → malloc("hello\n"), free("hel") → msgs[fd] = "hello\n"

extract_message → yields "hello\n", msgs[fd] = ""
```

The old `buf` is always freed inside `str_join` — no leak.

---

## The recv buffer sizing

```c
char buf_read[4097];
...
int read_bytes = recv(fd, buf_read, sizeof(buf_read) - 1, 0);
buf_read[read_bytes] = '\0';
```

- Array is 4097 bytes
- We tell `recv` to write max 4096 bytes (`sizeof - 1`)
- `buf_read[read_bytes]` is always `≤ buf_read[4096]` — valid, within bounds
- Without the `-1`, recv could fill all 4097 bytes and `buf_read[4097]` would be **out of bounds**

---

## Common mistakes to avoid

| Mistake | Effect |
|---------|--------|
| `rfds = afds = wfds` instead of `rfds = wfds = afds` | Copies uninitialized `wfds` into `afds`, destroys master set |
| Forgetting `break` after accept/remove | Continues loop with stale rfds, may double-process or crash |
| `recv(fd, buf, sizeof(buf), 0)` without `-1` | Off-by-one, writes null terminator out of bounds |
| Sending to `afds` instead of `wfds` | May block on a lazy client, freezing server |
| `ids[]` / `msgs[]` sized at 4096 or 4097 | fd numbers can go up to 65535 |
| Forgetting `FD_SET(sockfd, &afds)` after socket() | select() never sees the server fd, no connections accepted |
| Forgetting `max_fd = sockfd` after socket() | select() called with nfds=1, only scans fd 0 |
