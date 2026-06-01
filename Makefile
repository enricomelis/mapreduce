CC       = gcc
AR       = ar
ARFLAGS  = rcs
CFLAGS   = -Wall -Wextra -pedantic -std=c11
CPPFLAGS = -Iinclude

LIB_NAME  = libmr.a
SRC_DIR   = src
BUILD_DIR = build
TEST_DIR  = tests

LIB_SRCS = $(wildcard $(SRC_DIR)/*.c)
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
TEST_ATTR = $(BUILD_DIR)/test_attr
TEST_LIFECYCLE = $(BUILD_DIR)/test_lifecycle
TEST_START = $(BUILD_DIR)/test_start

.PHONY: all test clean

all: $(LIB_NAME)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB_NAME): $(LIB_OBJS)
	$(AR) $(ARFLAGS) $@ $^

test: $(TEST_ATTR) $(TEST_LIFECYCLE) $(TEST_START)
	./$(TEST_ATTR)
	./$(TEST_LIFECYCLE)
	./$(TEST_START)

$(TEST_ATTR): $(TEST_DIR)/test_attr.c $(LIB_NAME) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

$(TEST_LIFECYCLE): $(TEST_DIR)/test_lifecycle.c $(LIB_NAME) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

$(TEST_START): $(TEST_DIR)/test_start.c $(LIB_NAME) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

clean:
	rm -f $(LIB_NAME) $(LIB_OBJS) $(TEST_ATTR) $(TEST_LIFECYCLE) $(TEST_START)
