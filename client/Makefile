DS_PATH = $(HOME)/Documents/Embedded_and_RTOS/DS

CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -I$(DS_PATH)/HashMap \
          -I../common
LDFLAGS = -L$(DS_PATH) -lds -Wl,-rpath,$(DS_PATH) -lrt

CLIENT_SRCS = client_main.c client_mng.c client_net.c \
              client_groups_mng.c ui.c \
              ../common/protocol.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)
CLIENT_BIN  = chat_client

MC_SENDER_BIN   = mc_sender
MC_RECEIVER_BIN = mc_receiver

all: $(CLIENT_BIN) $(MC_SENDER_BIN) $(MC_RECEIVER_BIN)

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(MC_SENDER_BIN): mc_sender.c
	$(CC) $(CFLAGS) -o $@ $< -lrt

$(MC_RECEIVER_BIN): mc_receiver.c
	$(CC) $(CFLAGS) -o $@ $< -lrt

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test_ui: test_ui.c ui.c
	$(CC) $(CFLAGS) -o $@ $^
	./test_ui

clean:
	rm -f $(CLIENT_OBJS) $(CLIENT_BIN) $(MC_SENDER_BIN) $(MC_RECEIVER_BIN) test_ui

.PHONY: all clean test_ui