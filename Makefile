CC      = gcc
CFLAGS  = -fsanitize=address -Wall -Wextra -O2 -I. -D_GNU_SOURCE
LDFLAGS = -fsanitize=address -lsqlite3 -lpthread -lcjson $(shell pkg-config --libs libuv)

SRCS = http_utils.c server_functions.c route_handler.c report.c template.c \
       hash_table.c db.c user.c session.c client_pool.c geo.c picohttpparser.c

TARGET = segnalacity

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)

cleandb:
	rm -f segnalacity.db sessions.bin

.PHONY: clean cleandb
