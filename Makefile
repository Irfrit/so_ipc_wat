CC = gcc
CFLAGS = -Wall -Wextra -g
TARGETS = main p1 p2 p3

.PHONY: all clean

all: $(TARGETS)

main: main.c common.h
	$(CC) $(CFLAGS) -o main main.c

p1: p1.c common.h
	$(CC) $(CFLAGS) -o p1 p1.c

p2: p2.c common.h
	$(CC) $(CFLAGS) -o p2 p2.c

p3: p3.c common.h
	$(CC) $(CFLAGS) -o p3 p3.c

clean:
	rm -f $(TARGETS)
	rm -f /tmp/fifo_p1 /tmp/fifo_p2 /tmp/fifo_p3 /tmp/fifo_main /tmp/fifo_ready
	ipcrm -a 2>/dev/null || true

.SILENT: clean
