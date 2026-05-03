NAME	= mini_serv

CC	= gcc
CFLAGS	= -Wall -Wextra -Werror

SRC	= mini_serv.c

all: $(NAME)

$(NAME): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(NAME)

clean:
	rm -f $(NAME)

re: clean all

.PHONY: all clean re
