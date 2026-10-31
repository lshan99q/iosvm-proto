# SPDX-License-Identifier: MIT
#
# IOSVM wire protocol, v0.1. Buildable with cc and make, no dependencies.
#
#   make            build/libiosvm_proto.a
#   make test       build and run the test suite
#   make test-asan  same, with AddressSanitizer and UndefinedBehaviorSanitizer
#   make clean      remove build/
#   make help       this list

CC ?= cc
AR ?= ar

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -O2 -g
CPPFLAGS += -Iinclude

DEPFLAGS := -MMD -MP

SANFLAGS := -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -O1 -g \
            -fno-omit-frame-pointer -fsanitize=address,undefined

BUILD := build
ASAN_BUILD := $(BUILD)/asan

LIB_SRCS := src/ring.c src/codec.c src/fake_gpu.c
TEST_SRCS := tests/main.c \
             tests/test_proto.c \
             tests/test_ring.c \
             tests/test_codec.c \
             tests/test_fake_gpu.c \
             tests/test_integration.c \
             tests/test_ring_fuzz.c

LIB_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRCS))
LIB := $(BUILD)/libiosvm_proto.a
TEST_BIN := $(BUILD)/iosvm_tests

ASAN_LIB_OBJS := $(patsubst %.c,$(ASAN_BUILD)/%.o,$(LIB_SRCS))
ASAN_TEST_OBJS := $(patsubst %.c,$(ASAN_BUILD)/%.o,$(TEST_SRCS))
ASAN_LIB := $(ASAN_BUILD)/libiosvm_proto.a
ASAN_TEST_BIN := $(ASAN_BUILD)/iosvm_tests

.PHONY: all test test-asan clean help

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_OBJS) $(LIB) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(ASAN_LIB): $(ASAN_LIB_OBJS)
	$(AR) rcs $@ $^

$(ASAN_TEST_BIN): $(ASAN_TEST_OBJS) $(ASAN_LIB)
	$(CC) $(SANFLAGS) $(LDFLAGS) $(ASAN_TEST_OBJS) $(ASAN_LIB) -o $@

$(ASAN_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SANFLAGS) $(DEPFLAGS) -c $< -o $@

test-asan: $(ASAN_TEST_BIN)
	./$(ASAN_TEST_BIN)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(ASAN_LIB_OBJS:.o=.d) $(ASAN_TEST_OBJS:.o=.d)

help:
	@echo "IOSVM protocol v0.1"
	@echo "  make            build $(LIB)"
	@echo "  make test       build and run the test suite"
	@echo "  make test-asan  build and run the test suite under ASan+UBSan"
	@echo "  make clean      remove $(BUILD)/"
	@echo "  make help       show this help"
