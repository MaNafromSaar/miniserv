/*
** EXAM RANK 06 — mini_serv
** Annotated reference solution
**
** KEY CONCEPTS:
**   1. select() to multiplex I/O on multiple fds without blocking
**   2. Three fd_sets: afds (master), rfds (read-ready), wfds (write-ready)
**   3. Per-fd message buffer — partial messages accumulate until '\n'
**   4. str_join / extract_message from the provided main.c (copy them verbatim)
**
** ALLOWED FUNCTIONS:
**   write, close, select, socket, accept, listen, send, recv, bind,
**   strstr, malloc, realloc, free, calloc, bzero, atoi, sprintf,
**   strlen, exit, strcpy, strcat, memset
**
** FORBIDDEN: #define macros, printf, fprintf
*/

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>         /* sprintf only — printf is forbidden */
#include <netinet/ip.h>    /* sockaddr_in, htonl, htons, SOMAXCONN */

/* --- GLOBALS ---
**
** Using fd index directly as client index (fd < 65536) avoids a linked list.
** ids[]  : logical client id (0, 1, 2…) per fd
** msgs[] : per-fd heap-allocated buffer for partial (incomplete) messages
** afds   : master fd_set — always reflects which fds are open
** rfds / wfds : copies of afds given to select() each iteration (select modifies them)
** buf_write[100]: generous size — "server: client %d just arrived\n" is ~35 chars max
*/
int		count = 0, max_fd = 0;
int		ids[65536];
char	*msgs[65536];

fd_set	rfds, wfds, afds;
char	buf_read[1001], buf_write[100];


/* --- GIVEN HELPER: extract_message ---
**
** Scans *buf for the first '\n'. If found:
**   - *msg  = pointer to old *buf (the substring up to and including '\n')
**   - *buf  = newly allocated string with everything AFTER the '\n'
** Returns 1 if a complete message was extracted, 0 if none yet, -1 on alloc error.
**
** USAGE: call in a while loop — a single recv() may deliver several lines at once.
*/
int extract_message(char **buf, char **msg)
{
	char	*newbuf;
	int	i;

	*msg = 0;
	if (*buf == 0)
		return (0);
	i = 0;
	while ((*buf)[i])
	{
		if ((*buf)[i] == '\n')
		{
			newbuf = calloc(1, sizeof(*newbuf) * (strlen(*buf + i + 1) + 1));
			if (newbuf == 0)
				return (-1);
			strcpy(newbuf, *buf + i + 1);
			*msg = *buf;
			(*msg)[i + 1] = 0;
			*buf = newbuf;
			return (1);
		}
		i++;
	}
	return (0);
}

/* --- GIVEN HELPER: str_join ---
**
** Concatenates buf + add into a new heap allocation, frees old buf.
** Used to append each recv() chunk onto the per-client message buffer.
** Handles buf == NULL (first chunk for that client).
*/
char *str_join(char *buf, char *add)
{
	char	*newbuf;
	int		len;

	if (buf == 0)
		len = 0;
	else
		len = strlen(buf);
	newbuf = malloc(sizeof(*newbuf) * (len + strlen(add) + 1));
	if (newbuf == 0)
		return (0);
	newbuf[0] = 0;
	if (buf != 0)
		strcat(newbuf, buf);
	free(buf);
	strcat(newbuf, add);
	return (newbuf);
}


/* --- fatal_error ---
** Write to stderr (fd 2) and exit. Called for any syscall failure at startup.
*/
void	fatal_error()
{
	write(2, "Fatal error\n", 12);
	exit(1);
}

/* --- notify_other ---
** Sends str to every fd that is ready-to-write (in wfds) except the author.
** wfds is refreshed from afds every select() loop — only open fds are in it.
**
** WHY wfds and not afds?
**   select() fills wfds with fds that won't block on send(). Using afds
**   directly could block the server if a client's send buffer is full.
**   The subject says: "client can be lazy… you must NOT disconnect them"
**   so we trust select() to gate the send.
*/
void	notify_other(int author, char *str)
{
	for (int fd = 0; fd <= max_fd; fd++)
	{
		if (FD_ISSET(fd, &wfds) && fd != author)
			send(fd, str, strlen(str), 0);
	}
}

/* --- register_client ---
** Called when accept() returns a new fd.
** - Updates max_fd if new fd is larger
** - Assigns next logical id (count++)
** - Initialises msg buffer to NULL
** - Adds fd to afds (master set)
** - Broadcasts "server: client %d just arrived\n" to all existing clients
*/
void	register_client(int fd)
{
	max_fd = fd > max_fd ? fd : max_fd;
	ids[fd] = count++;
	msgs[fd] = NULL;
	FD_SET(fd, &afds);
	sprintf(buf_write, "server: client %d just arrived\n", ids[fd]);
	notify_other(fd, buf_write);
}

/* --- remove_client ---
** Called when recv() returns 0 or negative (disconnect / error).
** - Broadcasts "server: client %d just left\n"
** - Frees heap buffer (may be NULL if client never sent anything — free(NULL) is safe)
** - Removes from master set and closes fd
** NOTE: max_fd is NOT decremented — that's fine; select() handles sparse sets.
*/
void	remove_client(int fd)
{
	sprintf(buf_write, "server: client %d just left\n", ids[fd]);
	notify_other(fd, buf_write);
	free(msgs[fd]);
	FD_CLR(fd, &afds);
	close(fd);
}

/* --- send_msg ---
** Drains all complete lines from msgs[fd] (there may be multiple after one recv).
** For each complete line: sends "client %d: " prefix then the line itself.
** extract_message returns the line in *msg (heap-allocated); free it after sending.
*/
void	send_msg(int fd)
{
	char *msg;

	while (extract_message(&(msgs[fd]), &msg))
	{
		sprintf(buf_write, "client %d: ", ids[fd]);
		notify_other(fd, buf_write);
		notify_other(fd, msg);
		free(msg);
	}
}

/* --- create_socket ---
** Creates the TCP server socket.
** Reuses max_fd as a temp variable here — the socket fd becomes the
** initial max_fd (no client fds exist yet, so this is correct).
** Adds the server socket to afds immediately.
*/
int		create_socket()
{
	max_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (max_fd < 0)
		fatal_error();
	FD_SET(max_fd, &afds);
	return max_fd;
}

/* --- main ---
** Flow:
**   1. Validate args, FD_ZERO afds, create + bind + listen
**   2. Loop forever:
**        a. Copy afds → rfds and wfds  (select() will modify them)
**        b. Call select() — blocks until at least one fd is ready
**        c. Iterate fds 0…max_fd:
**             • server fd ready → accept new client, register, break
**             • client fd ready → recv data; if disconnected remove_client,
**               else append to buffer, extract & forward complete messages
**           break after handling ONE event per select() iteration —
**           safe because the loop restarts and select() re-evaluates.
*/
int		main(int ac, char **av)
{
	if (ac != 2)
	{
		write(2, "Wrong number of arguments\n", 26);
		exit(1);
	}

	FD_ZERO(&afds);
	int sockfd = create_socket();

	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(2130706433); /* 127.0.0.1 as 32-bit int */
	servaddr.sin_port = htons(atoi(av[1]));

	if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)))
		fatal_error();
	if (listen(sockfd, SOMAXCONN))
		fatal_error();

	while (1)
	{
		/* Reset rfds/wfds each iteration — select() clobbers them */
		rfds = wfds = afds;

		if (select(max_fd + 1, &rfds, &wfds, NULL, NULL) < 0)
			fatal_error();

		for (int fd = 0; fd <= max_fd; fd++)
		{
			if (!FD_ISSET(fd, &rfds))
				continue;

			if (fd == sockfd)
			{
				/* New connection */
				socklen_t addr_len = sizeof(servaddr);
				int client_fd = accept(sockfd, (struct sockaddr *)&servaddr, &addr_len);
				if (client_fd >= 0)
				{
					register_client(client_fd);
					break ; /* restart select loop — afds changed */
				}
			}
			else
			{
				/* Existing client sent data or disconnected */
				int read_bytes = recv(fd, buf_read, 1000, 0);
				if (read_bytes <= 0)
				{
					remove_client(fd);
					break ; /* restart — afds changed */
				}
				buf_read[read_bytes] = '\0';
				msgs[fd] = str_join(msgs[fd], buf_read);
				send_msg(fd);
				/* no break here — could handle more ready fds before next select */
			}
		}
	}
	return 0;
}
