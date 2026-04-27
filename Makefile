CC      = gcc
CFLAGS  = -fsanitize=address -Wall -Wextra -O2 -I.
LDFLAGS = -lsqlite3 -lpthread

SRCS = server_functions.c route_handler.c report.c \
       hash_table.c db.c user.c session.c client_pool.c

TARGET = segnalacity

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET) segnalacity.db

.PHONY: clean
