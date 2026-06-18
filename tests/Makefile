CC      ?= gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wno-format-truncation
CFLAGS  ?= $(CSTD) $(WARN) -O0 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I../src

SRC_DIR   := ../src
BUILD_DIR := build

UNIT_BIN := $(BUILD_DIR)/test_parser $(BUILD_DIR)/test_files

.PHONY: unit clean

unit: $(UNIT_BIN)
	@for t in $(UNIT_BIN); do echo "== $$t =="; ./$$t || exit 1; done

$(BUILD_DIR)/test_parser: test_parser.c $(SRC_DIR)/http_parser.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/test_files: test_files.c $(SRC_DIR)/files.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
