# mini_serv — exam roadmap (starting from given main.c)

## Given main.c already contains
- all includes
- `extract_message()`
- `str_join()`
- socket / bzero / bind / listen skeleton with `printf` + `exit(0)` blocks

---

## Step 1 — Add globals (above the given helpers)

```c
int  count = 0, max_fd = 0;
int  ids[65536];
char *msgs[65536];

fd_set rfds, wfds, afds;
char   buf_read[4097];
char   buf_write[128];
```

---

## Step 2 — Add 5 functions (between helpers and main)

```
fatal_error       write(2, "Fatal error\n", 12) + exit(1)

notify_other      loop 0..max_fd
                    FD_ISSET(fd, &wfds) && fd != author → send

register_client   max_fd = fd > max_fd ? fd : max_fd
                  ids[fd] = count++
                  msgs[fd] = NULL
                  FD_SET(fd, &afds)
                  sprintf + notify_other

remove_client     sprintf + notify_other
                  free(msgs[fd])
                  FD_CLR(fd, &afds)
                  close(fd)

send_msg          while (extract_message(&msgs[fd], &msg))
                    sprintf prefix → notify_other
                    notify_other msg
                    free(msg)
```

---

## Step 3 — Gut main, keep the struct, fix everything

### Args check (replace the empty main header)
```c
if (argc != 2) { write(2, "Wrong number of arguments\n", 26); exit(1); }
FD_ZERO(&afds);
```

### Socket (replace printf block)
```c
sockfd = socket(AF_INET, SOCK_STREAM, 0);
if (sockfd == -1) fatal_error();
max_fd = sockfd;
FD_SET(sockfd, &afds);
```

### servaddr — keep bzero + sin_family + sin_addr + sin_port, just fix port:
```c
servaddr.sin_port = htons(atoi(argv[1]));
```

### bind / listen (replace printf blocks)
```c
if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) != 0)
    fatal_error();
if (listen(sockfd, 10) != 0)
    fatal_error();
```

### Drop from main
- `connfd`, `len`, `cli` variables
- the `accept()` + printf block at the bottom

---

## Step 4 — Add select loop (replaces the dropped accept block)

```c
while (1)
{
    rfds = wfds = afds;                          // reset before every select

    if (select(max_fd + 1, &rfds, &wfds, NULL, NULL) < 0)
        fatal_error();

    for (int fd = 0; fd <= max_fd; fd++)
    {
        if (!FD_ISSET(fd, &rfds))
            continue;

        if (fd == sockfd)
        {
            int client_fd = accept(sockfd, NULL, NULL);  // don't need client addr
            if (client_fd >= 0)
            {
                register_client(client_fd);
                break;                           // afds changed → restart loop
            }
        }
        else
        {
            int n = recv(fd, buf_read, 4096, 0);
            if (n <= 0)
            {
                remove_client(fd);
                break;                           // afds changed → restart loop
            }
            buf_read[n] = '\0';
            msgs[fd] = str_join(msgs[fd], buf_read);
            send_msg(fd);
        }
    }
}
```

---

## Key rules to never forget

| Rule | Why |
|------|-----|
| `rfds = wfds = afds` every iteration | select() clobbers them |
| `break` after accept & disconnect | afds changed, loop must restart |
| `accept(fd, NULL, NULL)` | client address not needed |
| send only inside `FD_ISSET(&wfds)` | keeps server non-blocking |
| no `#define` | subject forbids it |
| errors to `fd 2` (stderr) | subject requires it |
| `buf_read[4097]`, recv max `4096` | room for null terminator |

---

## Test commands

```bash
# Terminal 1
./mini_serv 4242

# Terminal 2 & 3
nc 127.0.0.1 4242

# Multi-line test
printf "line1\nline2\n" | nc -q1 127.0.0.1 4242
```
