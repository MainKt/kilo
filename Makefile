CWARNFLAGS= -Wall -Wextra -Wpedantic -Wshadow -g
CFLAGS= -fsanitize=address,undefined
LDFLAGS+=	-fsanitize=address,undefined

kilo: kilo.c
	$(CC) $(CFLAGS) $(LDFLAGS) $(CWARNFLAGS) kilo.c -o kilo
