#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

//remove printf! always lead to fatal error
//globals, fatal_error, notify_other, register_client, remove_client, sendmsg, create_socket
int count = 0;
int max_fd = 0;
int ids[65536];
char* msgs[65536];

fd_set rfds,wfds, afds;
char buf_read[4097];
char buf_write[128];

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
		return (0);
	newbuf[0] = 0;
	if (buf != 0)
		strcat(newbuf, buf);
	free(buf);
	strcat(newbuf, add);
	return (newbuf);
}

void fatal_error()
{
	write(2, "Fatal error\n", 12);
	exit(1);
}

void notify_other(int author, char* string)
{
	for (int i = 0; i <= max_fd; i++)
	{
		if (FD_ISSET(i, &wfds) && i != author)
			send(i, string, strlen(string), 0);
	}
	
}

void register_client(int fd)
{
	max_fd = fd > max_fd ? fd : max_fd;
	ids[fd] = count++;
	msgs[fd] = NULL;
	FD_SET(fd, &afds);
	sprintf(buf_write, "server: client %d just arrived\n", ids[fd]);
	notify_other(fd, buf_write);
}

void remove_client(int fd)
{
	sprintf(buf_write, "server: client %d just left\n", ids[fd]);
	notify_other(fd, buf_write);
	free(msgs[fd]);
	FD_CLR(fd, &afds);
	close(fd);
}

void send_msg(int fd)
{
	char *msg;
	while(extract_message(&msgs[fd], &msg))
	{
		sprintf(buf_write, "client %d: ", ids[fd]);
		notify_other(fd, buf_write);
		notify_other(fd, msg);
		free(msg);
	}
}




int main(int argc, char** argv) 
{
	int sockfd;
	struct sockaddr_in servaddr; 

	if (argc != 2)
	{
		write(2, "Wrong number of arguments\n", 26);
		exit(1);
	}
	FD_ZERO(&afds);
	bzero(&servaddr, sizeof(servaddr)); 
	// socket create and verification 
	sockfd = socket(AF_INET, SOCK_STREAM, 0); 
	if (sockfd == -1) 
		fatal_error();
	max_fd = sockfd;
	FD_SET(sockfd, &afds);

	// assign IP, PORT 
	servaddr.sin_family = AF_INET; 
	servaddr.sin_addr.s_addr = htonl(2130706433); //127.0.0.1
	servaddr.sin_port = htons(atoi(argv[1])); 

	// Binding newly created socket to given IP and verification 
	if ((bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr))) != 0)
		fatal_error();
	if (listen(sockfd, 10) != 0) 
		fatal_error();
	
	//MAIN LOOP
	while(1)
	{
		rfds = wfds = afds;
		if (select(max_fd +1, &rfds, &wfds, NULL, NULL) < 0)
			fatal_error();
		for (int fd = 0; fd <= max_fd; fd++)
		{
			if (!FD_ISSET(fd, &rfds))
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
				int read_bytes = recv(fd, buf_read, sizeof(buf_read) - 1, 0);
				if (read_bytes <= 0)
				{
					remove_client(fd);
					break;
				}
				buf_read[read_bytes] = '\0';
				msgs[fd] = str_join(msgs[fd], buf_read);
				send_msg(fd);
			}
		}
	}
	return 0;
}