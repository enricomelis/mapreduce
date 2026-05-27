CC       = gcc
AR       = ar
ARFLAGS  = rcs
CFLAGS   = -Wall -Wextra -pedantic -std=c11
CPPFLAGS = -Iinclude

LIB_NAME  = libmr.a
SRC_DIR   = src
BUILD_DIR = build

LIB_SRCS = $(wildcard $(SRC_DIR)/*.c)
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))

.PHONY: all test clean

all: $(LIB_NAME)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB_NAME): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

test: $(LIB_NAME)
	@echo "Test non ancora implementati"

clean:
	rm -f $(LIB_NAME) $(LIB_OBJS)
