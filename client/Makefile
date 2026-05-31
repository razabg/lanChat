DS_PATH = $(HOME)/projectsLinux/DS

CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -I$(DS_PATH)/LinkedList/DoubleLinkedList \
          -I$(DS_PATH)/HashMap \
          -I../common
LDFLAGS = -L$(DS_PATH) -lds -Wl,-rpath,$(DS_PATH) -lrt

# ── Main client binary ───────────────────────────────────────────────────────
CLIENT_SRCS = client_main.c client_mng.c client_net.c client_groups_mng.c ui.c \
              ../common/protocol.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)
CLIENT_BIN  = chat_client

# ── Standalone mc_sender binary ──────────────────────────────────────────────
SENDER_SRCS = mc_sender.c
SENDER_OBJS = $(SENDER_SRCS:.c=.o)
SENDER_BIN  = mc_sender

# ── Standalone mc_receiver binary ────────────────────────────────────────────
RECEIVER_SRCS = mc_receiver.c
RECEIVER_OBJS = $(RECEIVER_SRCS:.c=.o)
RECEIVER_BIN  = mc_receiver

all: $(CLIENT_BIN) $(SENDER_BIN) $(RECEIVER_BIN)

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SENDER_BIN): $(SENDER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lrt

$(RECEIVER_BIN): $(RECEIVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lrt

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(CLIENT_OBJS) $(SENDER_OBJS) $(RECEIVER_OBJS) \
	      $(CLIENT_BIN) $(SENDER_BIN) $(RECEIVER_BIN)

.PHONY: all clean
