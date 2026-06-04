#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

int next_id;
int max_fd;
int ids[65536];
char *bufs[65536];
fd_set all, readfds, writefds;
char rbuf[4097];
char wbuf[128];

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
	{
		free(buf);
		return (0);
	}
	newbuf[0] = 0;
	if (buf != 0)
		strcat(newbuf, buf);
	strcat(newbuf, add);
	free(buf);
	return (newbuf);
}

void fatal_error(void)
{
	write(2, "Fatal error\n", 12);
	exit(1);
}

void broadcast_to_others(int sender_fd, char *s)
{
	for (int fd = 0; fd <= max_fd; fd++)
	{
		if (FD_ISSET(fd, &writefds) && fd != sender_fd)
			send(fd, s, strlen(s), 0);
	}
}

void register_client(int fd)
{
	max_fd = (fd > max_fd) ? fd : max_fd;
	ids[fd] = next_id++;
	bufs[fd] = 0;
	FD_SET(fd, &all);
	sprintf(wbuf, "server: client %d just arrived\n", ids[fd]);
	broadcast_to_others(fd, wbuf);
}

void remove_client(int fd)
{
	sprintf(wbuf, "server: client %d just left\n", ids[fd]);
	broadcast_to_others(fd, wbuf);
	free(bufs[fd]);
	FD_CLR(fd, &all);
	close(fd);
}

void flush_client_messages(int fd)
{
	char *msg;
	int status;

	while (1)
	{
		status = extract_message(&bufs[fd], &msg);
		if (status < 0)
			fatal_error();
		if (status == 0)
			break;
		sprintf(wbuf, "client %d: ", ids[fd]);
		broadcast_to_others(fd, wbuf);
		broadcast_to_others(fd, msg);
		free(msg);
	}
}




int main(int argc, char **argv)
{
	int sockfd;
	struct sockaddr_in servaddr;
	int fd;

	if (argc != 2)
	{
		write(2, "Wrong number of arguments\n", 26);
		exit(1);
	}
	FD_ZERO(&all);
	bzero(&servaddr, sizeof(servaddr));
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		fatal_error();
	max_fd = sockfd;
	FD_SET(sockfd, &all);

	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(2130706433);
	servaddr.sin_port = htons(atoi(argv[1]));

	if ((bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr))) != 0)
		fatal_error();
	if (listen(sockfd, 10) != 0)
		fatal_error();

	// One select loop: accept new clients, read data, split by '\n', broadcast lines.
	while (1)
	{
		readfds = writefds = all;
		if (select(max_fd + 1, &readfds, &writefds, 0, 0) < 0)
			fatal_error();
		for (fd = 0; fd <= max_fd; fd++)
		{
			if (!FD_ISSET(fd, &readfds))
				continue;
			if (fd == sockfd)
			{
				int client_fd = accept(sockfd, NULL, NULL);
				if (client_fd >= 0)
				{
					register_client(client_fd);
					break;
				}
			}
			else
			{
				int read_bytes = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
				if (read_bytes <= 0)
				{
					remove_client(fd);
					break;
				}
				rbuf[read_bytes] = '\0';
				bufs[fd] = str_join(bufs[fd], rbuf);
				if (!bufs[fd])
					fatal_error();
				flush_client_messages(fd);
			}
		}
	}
	return (0);
}