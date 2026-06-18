CC      ?= gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
CFLAGS  ?= $(CSTD) $(WARN) -O2 -g -D_GNU_SOURCE
LDFLAGS ?=

SRC_DIR   := src
BUILD_DIR := build
BIN       := $(BUILD_DIR)/httpd

SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean asan demo test unit-test

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean all

demo: all
	./$(BIN) -p 8080 -r www -v

unit-test:
	$(MAKE) -C tests unit

test: all unit-test
	python3 tests/integration_test.py ./$(BIN)

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C tests clean
