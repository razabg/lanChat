DS_PATH = $(HOME)/projectsLinux/DS

CC      = gcc
CFLAGS  = -Wall -Wextra -g \
          -I$(DS_PATH)/LinkedList/DoubleLinkedList \
          -I$(DS_PATH)/HashMap \
          -I$(DS_PATH)/GenQueue \
          -I../common
LDFLAGS = -L$(DS_PATH) -lds -Wl,-rpath,$(DS_PATH)

SRCS = server_main.c server_net.c server_mng.c user_mng.c group_mng.c \
       logger.c ../common/protocol.c
OBJS = $(SRCS:.c=.o)
TARGET = chat_server

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
