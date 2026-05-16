CC      = gcc
CFLAGS  = -fsanitize=address -Wall -Wextra -O2 -I. -D_GNU_SOURCE
LDFLAGS = -fsanitize=address -lsqlite3 -lpthread -lcjson $(shell pkg-config --libs libuv)

SRCS =  http_utils.c  server_functions.c  report.c route_handler.c template.c route_api.c route_pages.c route_helpers.c\
       hash_table.c db.c user.c session.c slab_allocator.c connection_manager.c geo.c picohttpparser.c

TARGET = segnalacity

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)

cleandb:
	rm -f segnalacity.db sessions.bin

.PHONY: clean cleandb
