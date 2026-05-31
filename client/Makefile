CC      = gcc
CFLAGS  = -Wall -Wextra -g

SRC     = ui.c
TEST_BIN = test_ui

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): test_ui.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TEST_BIN)

.PHONY: test clean
