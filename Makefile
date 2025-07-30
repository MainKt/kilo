CWARNFLAGS= -Wall -Wextra -Wpedantic -Wshadow -g
CFLAGS= -std=c2x -fsanitize=address,undefined
LDFLAGS+=	-fsanitize=address,undefined -lsbuf

kilo: kilo.c
	$(CC) $(CFLAGS) $(LDFLAGS) $(CWARNFLAGS) kilo.c -o kilo
