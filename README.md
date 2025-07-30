# kilo

A simple TUI text editor based on
[antirez's kilo](https://github.com/antirez/kilo).

- Uses a `kqueue(2)` based event loop
- Draws the TUI in an alternate buffer

## Dependencies
- [`kqueue(2)`](https://man.freebsd.org/cgi/man.cgi?kqueue(2))
- [`sbuf(9)`](https://man.freebsd.org/cgi/man.cgi?sbuf(3))
