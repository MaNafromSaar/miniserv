NAME	= miniserv

CC	= gcc
CFLAGS	= -Wall -Wextra -Werror

SRC	= miniserv.c

all: $(NAME)

$(NAME): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(NAME)

clean:
	rm -f $(NAME)

re: clean all

.PHONY: all clean re
