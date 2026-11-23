NAME    = spider
CC      = cc
CFLAGS  = -Wall -Wextra -Werror -D_GNU_SOURCE
SRC     = spider_OLD.c
OBJ     = $(SRC:.c=.o)

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) -o $(NAME) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -f $(OBJ)

fclean: clean
	@rm -f $(NAME)
	@echo "Cleaned! Don't forget to delete the folder of the images if you want to free up space."

re: fclean all

.PHONY: all clean fclean re