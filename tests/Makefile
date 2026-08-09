CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=

CPPFLAGS += -I../src
CFLAGS += -std=c11 -Wall -Wextra -Werror

BUILD_DIR := build
TEST_BINARY := $(BUILD_DIR)/test_modem_pack
TEST_SOURCES := test_modem_pack.c modem_stubs.c
TEST_OBJECTS := $(TEST_SOURCES:%.c=$(BUILD_DIR)/%.o)
MODEM_OBJECT := $(BUILD_DIR)/modem_pack.o
DEPENDENCIES := $(TEST_OBJECTS:.o=.d) $(MODEM_OBJECT:.o=.d)

.PHONY: all clean test

all: $(TEST_BINARY)

test: $(TEST_BINARY)
	./$(TEST_BINARY)

$(TEST_BINARY): $(TEST_OBJECTS) $(MODEM_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(MODEM_OBJECT): ../src/modem_pack.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPENDENCIES)
