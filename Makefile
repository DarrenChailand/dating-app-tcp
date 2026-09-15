CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
PORT ?= 4242

CPPFLAGS += -DPORT=$(PORT)
BIN_DIR := bin
SERVER := $(BIN_DIR)/needlove-server
CLIENT := $(BIN_DIR)/needlove-client

.PHONY: all clean debug run-server run-client test

all: $(SERVER) $(CLIENT)

$(BIN_DIR):
	mkdir -p $@

$(SERVER): src/server.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(CLIENT): src/client.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

debug: CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -g3 -O0 -fsanitize=address,undefined
debug: LDFLAGS := -fsanitize=address,undefined
debug: clean all

run-server: $(SERVER)
	./$(SERVER)

run-client: $(CLIENT)
	./$(CLIENT)

test: all
	python3 scripts/smoke_test.py

clean:
	rm -rf $(BIN_DIR)
