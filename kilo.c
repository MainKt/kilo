#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/_termios.h>
#include <termios.h>
#include <unistd.h>

struct termios orignal_termios;

static void enable_raw_mode(void);
static void disable_raw_mode(void);

int main(void) {
  enable_raw_mode();

  for (;;) {
    char c = '\0';

    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      err(EXIT_FAILURE, "read");

    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q')
      break;
  }

  return (0);
}

static void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orignal_termios) == -1)
    err(EXIT_FAILURE, "tcsetattr");
}

static void enable_raw_mode(void) {
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &orignal_termios) == -1)
    err(EXIT_FAILURE, "tcgetattr");

  atexit(disable_raw_mode);

  raw = orignal_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    err(EXIT_FAILURE, "tcsetattr");
}
