CC     = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11
NAME   = main
SRC_DIR = src
CPPFLAGS = -Iinclude
LIBS   = -pthread
OBJS   = $(SRC_DIR)/$(NAME).o
ARGS  ?=

.PHONY: run debug release clean

release: CFLAGS += -O2 -DNDEBUG
release: $(NAME)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CPPFLAGS) -c $(CFLAGS) $< -o $@

$(NAME): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

run: release
	./$(NAME) $(ARGS)

debug: CFLAGS += -O0 -g3
debug: $(NAME)
	gdb ./$(NAME)

clean:
	rm -f $(NAME) $(OBJS)
