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
PRIVATE_HDRS = $(wildcard $(SRC_DIR)/*.h)
TEST_ATTR_LIFECYCLE = $(BUILD_DIR)/test_attr_lifecycle
TEST_GENERIC_E2E = $(BUILD_DIR)/test_generic_e2e
TEST_DIRECTORY_CONCURRENCY_LOG = $(BUILD_DIR)/test_directory_concurrency_log
EXAMPLE_WORD_COUNT = $(BUILD_DIR)/word_count

.PHONY: all example test clean

all: $(LIB_NAME)

example: $(EXAMPLE_WORD_COUNT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(PRIVATE_HDRS) include/mr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB_NAME): $(LIB_OBJS)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $^

test: $(TEST_ATTR_LIFECYCLE) $(TEST_GENERIC_E2E) $(TEST_DIRECTORY_CONCURRENCY_LOG)
	./$(TEST_ATTR_LIFECYCLE)
	./$(TEST_GENERIC_E2E)
	./$(TEST_DIRECTORY_CONCURRENCY_LOG)

$(TEST_ATTR_LIFECYCLE): $(TEST_DIR)/test_attr_lifecycle.c $(LIB_NAME) include/mr.h $(PRIVATE_HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

$(TEST_GENERIC_E2E): $(TEST_DIR)/test_generic_e2e.c $(LIB_NAME) include/mr.h $(PRIVATE_HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

$(TEST_DIRECTORY_CONCURRENCY_LOG): $(TEST_DIR)/test_directory_concurrency_log.c $(LIB_NAME) include/mr.h $(PRIVATE_HDRS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

$(EXAMPLE_WORD_COUNT): examples/word_count.c $(LIB_NAME) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_NAME) -o $@

clean:
	rm -f $(LIB_NAME) $(BUILD_DIR)/*.o $(TEST_ATTR_LIFECYCLE) $(TEST_GENERIC_E2E) $(TEST_DIRECTORY_CONCURRENCY_LOG) $(EXAMPLE_WORD_COUNT) output.mro
