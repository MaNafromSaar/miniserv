# mini_serv.c — Code Walkthrough

---

## 1. The Buffers: Why so different?

```c
char rbuf[4097];   // read buffer
char wbuf[128];    // write buffer
```

They serve completely different purposes:

| Buffer | Size | Purpose |
|--------|------|---------|
| `rbuf` | 4097 bytes | Receives raw incoming data from a client via `recv()` |
| `wbuf` | 128 bytes | Holds formatted announcement strings built with `sprintf()` |

**`rbuf` is large** because you have no control over how much a client sends at once.
`recv()` pulls up to `sizeof(rbuf) - 1` bytes per call — the `-1` reserves space for the null terminator you manually add right after. 4096 is a classic page-size-aligned choice.

**`wbuf` is small** because its only job is to hold short, predictable server announcements:
```
"server: client 42 just arrived\n"   → ~35 chars max
"server: client 42 just left\n"      → ~30 chars max
"client 42: "                        → ~12 chars max
```
128 bytes is more than enough for any of those strings, even with a large client ID.

---

## 2. Why is `fd_set` marked red in VSCode?

`fd_set` is defined in `<sys/select.h>`. On Linux, it gets pulled in *transitively* through other system headers at **compile time**, which is why `gcc` has no problem with it.

VSCode's language server (IntelliSense / clangd) does **static analysis without compiling**. Without a `compile_commands.json` or a `.clangd` config pointing to your exact include paths, it doesn't always resolve transitive system headers. Result: red underline, even though the code compiles fine.

**Fix (optional):** add `#include <sys/select.h>` explicitly — it's good practice anyway.

---

## 3. `fd_set` — How and Why It Really Works

### What is it?

`fd_set` is a **fixed-size bitmask** (usually 1024 bits = 128 bytes on Linux).
Each bit position corresponds to a file descriptor number.

```
Bit 0 → fd 0 (stdin)
Bit 1 → fd 1 (stdout)
Bit 3 → fd 3 (your server socket)
Bit 7 → fd 7 (some client)
...
```

If a bit is **1**, that fd is "in the set". If **0**, it's not.

### Why Capital Letters for All Operations?

`FD_ZERO`, `FD_SET`, `FD_CLR`, `FD_ISSET` are all **C preprocessor macros**, not functions.
Macros are conventionally written in ALL_CAPS to warn you: *this is not a normal function call, it expands inline and may evaluate arguments more than once.*

```c
FD_ZERO(&all);          // set all 1024 bits to 0
FD_SET(sockfd, &all);   // set bit[sockfd] = 1
FD_CLR(fd, &all);       // set bit[fd] = 0
FD_ISSET(fd, &set);     // returns non-zero if bit[fd] == 1
```

Internally, `FD_SET(fd, set)` expands to something like:
```c
(set)->fds_bits[fd / NFDBITS] |= (1 << (fd % NFDBITS));
```
It's pure bitwise arithmetic on an array of `long` values. No system call, no overhead.

### The Three Sets in This Program

```c
fd_set readfds, writefds, all;
```

| Set | Role |
|-----|------|
| `all` | Master set — every fd currently tracked by the server. Never touched by `select()`. |
| `readfds` | Copy of `all` passed to `select()` — gets **modified** to show which fds are readable |
| `writefds` | Copy of `all` passed to `select()` — gets **modified** to show which fds are writable |

This copy-before-select pattern is **critical**: `select()` overwrites the sets it receives. If you passed `all` directly, you'd lose your master list on every iteration.

```c
// At the top of the main loop, every iteration:
readfds = writefds = all;   // restore the sets from master
```

---

## 4. The System Calls

### `select()`

```c
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
```

**Blocks** until at least one fd in your sets becomes ready for I/O.
After it returns, `readfds` and `writefds` no longer mean "all fds I care about" — they now mean **only the fds that are currently ready**.

```c
select(max_fd + 1, &readfds, &writefds, 0, 0);
//      ^           ^          ^          ^  ^
//  how many fds  readable?  writable?  except  timeout (NULL = block forever)
```

- `nfds` must be `max_fd + 1` because `select` checks bits 0 through `nfds-1`
- Passing `NULL` for timeout means "wait as long as needed"

After this call you loop over all fds and ask `FD_ISSET(fd, &readfds)` — now only the ready ones answer yes.

### `accept()`

```c
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
```

When the **listening socket** (not a client fd) becomes readable, it means a new connection is waiting. `accept()` **dequeues** it and hands you a brand new fd representing that specific client.

```c
int client_fd = accept(sockfd, NULL, NULL);
```

`NULL, NULL` means we don't care about the client's IP/port. The return value is the new client's fd, which we then pass to `register_client()`.

### `recv()`

```c
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
```

Reads up to `len` bytes from a socket into `buf`. Like `read()` but socket-specific.
- Returns the number of bytes actually received (can be less than `len`)
- Returns `0` if the client closed the connection
- Returns `-1` on error
- `flags = 0` means standard blocking read

```c
int read_bytes = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
if (read_bytes <= 0)
    remove_client(fd);   // 0 = disconnect, <0 = error — both mean: clean up
rbuf[read_bytes] = '\0'; // manually null-terminate what we got
```

### `send()`

```c
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
```

Sends `len` bytes from `buf` to a socket. Like `write()` but socket-specific.
- `flags = 0` means standard blocking send

```c
send(fd, s, strlen(s), 0);
```

Used inside `broadcast_others()` to push a message to every fd **except** the sender.

---

## 5. The Four Core Functions

### `broadcast_others(int sender_fd, char *s)`

```c
void broadcast_others(int sender_fd, char *s)
{
    for (int fd = 0; fd <= max_fd; fd++)
    {
        if (FD_ISSET(fd, &writefds) && fd != sender_fd)
            send(fd, s, strlen(s), 0);
    }
}
```

Iterates every possible fd from 0 to `max_fd`. For each:
- `FD_ISSET(fd, &writefds)` — is this fd currently connected and writable? (excludes the server socket implicitly — if it's not a client, it won't be in writefds)
- `fd != sender_fd` — skip the one who sent the message

Note it uses `writefds`, not `all` or `readfds`. This is the key safety check: `select()` already confirmed these fds are ready to receive data without blocking.

### `register_client(int fd)`

```c
void register_client(int fd)
{
    max_fd = (fd > max_fd) ? fd : max_fd;  // extend the scan range if needed
    ids[fd] = next_id++;                    // assign human-readable ID
    bufs[fd] = 0;                           // no pending message data yet
    FD_SET(fd, &all);                       // add to master set
    sprintf(wbuf, "server: client %d just arrived\n", ids[fd]);
    broadcast_others(fd, wbuf);             // tell everyone else
}
```

Called right after `accept()` returns a new `client_fd`.
- `ids[]` maps fd → sequential ID (0, 1, 2, …). The fd itself can be reused by the OS after a disconnect, so you need your own stable IDs.
- `bufs[]` is the per-client message accumulator. Starts as NULL.
- `max_fd` must be updated so the `select()` call and the fd-scan loop cover the new fd.

### `remove_client(int fd)`

```c
void remove_client(int fd)
{
    sprintf(wbuf, "server: client %d just left\n", ids[fd]);
    broadcast_others(fd, wbuf);   // notify before cleanup
    free(bufs[fd]);                // release any pending buffered data
    FD_CLR(fd, &all);              // remove from master set
    close(fd);                     // release the fd back to the OS
}
```

Notice: the goodbye message is sent **before** `FD_CLR` and `close`. Order matters — once you close the fd it's gone.

Note: `max_fd` is not decreased here. That's fine — the loop will just see an fd that isn't in any set and skip it. Lowering `max_fd` would be an optimization, not a correctness requirement.

### `flush_messages(int fd)`

```c
void flush_messages(int fd)
{
    char *msg;
    int status;

    while(1)
    {
        status = extract_message(&bufs[fd], &msg);
        if (status < 0)  fatal_error();   // malloc failed inside extract_message
        if (status == 0) break;           // no complete message yet, wait for more data

        sprintf(wbuf, "client %d: ", ids[fd]);
        broadcast_others(fd, wbuf);   // send the "client N: " prefix
        broadcast_others(fd, msg);    // then send the message itself
        free(msg);
    }
}
```

Why a loop? TCP is a **stream protocol**. A single `recv()` might give you:
- Part of a message (no `\n` yet) → `extract_message` returns 0, buffer it and wait
- Exactly one message → loop runs once and exits
- Two messages packed together → loop runs twice, extracts both

The per-client `bufs[fd]` accumulates data across multiple `recv()` calls until `\n` is found. `extract_message` slices off one complete line at a time, leaving the rest in the buffer for the next call.

---

## 6. Main Loop Summary

```
startup:
  create listening socket → bind → listen

loop:
  readfds = writefds = all           ← reset from master set each iteration
  select(...)                        ← BLOCK until something happens

  for fd 0..max_fd:
    if fd not readable: skip

    if fd == sockfd:                 ← new connection knocking
      accept() → client_fd
      register_client(client_fd)
      break                          ← restart loop (one event at a time)

    else:                            ← existing client sent data
      recv() into rbuf
      if recv <= 0: remove_client(fd), break
      append rbuf to bufs[fd]
      flush_messages(fd)             ← broadcast any complete lines
```

The `break` after `register_client` / `remove_client` is important: the `for` loop iterates over the **pre-select** state of fds. After adding or removing a client, that state is stale. Breaking out restarts the main loop, which calls `select()` again and gets a fresh, consistent view.
