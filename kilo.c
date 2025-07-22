#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_termios.h>
#include <sys/_winsize.h>
#include <sys/ioccom.h>
#include <sys/queue.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"

struct editor_config {
  int cursor_x, cursor_y;
  int screen_rows, screen_cols;
  struct termios orignal_termios;
};

static struct editor_config editor_config;

enum editor_key {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

struct append_buffer {
  char *b;
  int len;
};

#define AB_INIT(ab)                                                            \
  do {                                                                         \
    struct append_buffer *_AB_ab = (ab);                                       \
    _AB_ab->b = NULL;                                                          \
    _AB_ab->len = 0;                                                           \
  } while (0)

#define AB_APPEND(ab, s, slen)                                                 \
  do {                                                                         \
    struct append_buffer *_AB_ab = (ab);                                       \
    int _AB_slen = slen;                                                       \
    char *_AB_new = realloc(_AB_ab->b, _AB_ab->len + _AB_slen);                \
    if (_AB_new == NULL)                                                       \
      break;                                                                   \
    memcpy(&_AB_new[_AB_ab->len], s, _AB_slen);                                \
    _AB_ab->b = _AB_new;                                                       \
    _AB_ab->len += _AB_slen;                                                   \
  } while (0)

#define AB_FREE(ab)                                                            \
  do {                                                                         \
    struct append_buffer *_AB_ab = (ab);                                       \
    free(_AB_ab->b);                                                           \
    _AB_ab->len = 0;                                                           \
  } while (0)

static void die(const char *);
static void enable_raw_mode(void);
static void disable_raw_mode(void);
static int editor_read_key(void);
static int get_cursor_position(int *rows, int *cols);
static int get_window_size(int *rows, int *cols);

static void editor_move_cursor(int key);
static void editor_process_keypress(void);

static void editor_refresh_screen(void);
static void editor_draw_rows(struct append_buffer *ab);

static void init_editor(void);

int main(void) {
  enable_raw_mode();
  init_editor();

  for (;;) {
    editor_refresh_screen();
    editor_process_keypress();
  }

  return (0);
}

static void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 4);

  perror(s);
  exit(EXIT_FAILURE);
}

static void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor_config.orignal_termios) == -1)
    die("tcsetattr");
}

static void enable_raw_mode(void) {
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &editor_config.orignal_termios) == -1)
    die("tcgetattr");

  atexit(disable_raw_mode);

  raw = editor_config.orignal_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

static int editor_read_key(void) {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return ('\x1b');
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return ('\x1b');

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return ('\x1b');

        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return (HOME_KEY);
          case '3':
            return (DEL_KEY);
          case '4':
            return (END_KEY);
          case '5':
            return (PAGE_UP);
          case '6':
            return (PAGE_DOWN);
          case '7':
            return (HOME_KEY);
          case '8':
            return (END_KEY);
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return (ARROW_UP);
        case 'B':
          return (ARROW_DOWN);
        case 'D':
          return (ARROW_LEFT);
        case 'C':
          return (ARROW_RIGHT);
        case 'H':
          return (HOME_KEY);
        case 'F':
          return (END_KEY);
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return (HOME_KEY);
      case 'F':
        return (END_KEY);
      }
    }

    return ('\x1b');
  }

  return (c);
}

static int get_cursor_position(int *rows, int *cols) {
  char buf[32] = "";

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return (-1);

  for (size_t i = 0; i < sizeof(buf) - 1; i++) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1 || buf[i] == 'R')
      break;
  }

  if (buf[0] != '\x1b' || buf[1] != '[' ||
      sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return (-1);

  return (0);
}

static int get_window_size(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return (-1);
    return (get_cursor_position(rows, cols));
  }

  *cols = ws.ws_col;
  *rows = ws.ws_row;

  return (0);
}

static void editor_move_cursor(int key) {
  switch (key) {
  case ARROW_LEFT:
    if (editor_config.cursor_x != 0)
      editor_config.cursor_x--;
    break;
  case ARROW_RIGHT:
    if (editor_config.cursor_x != editor_config.screen_cols - 1)
      editor_config.cursor_x++;
    break;
  case ARROW_UP:
    if (editor_config.cursor_y != 0)
      editor_config.cursor_y--;
    break;
  case ARROW_DOWN:
    if (editor_config.cursor_y != editor_config.screen_rows - 1)
      editor_config.cursor_y++;
    break;
  }
}

static void editor_process_keypress(void) {
  int c = editor_read_key();

  switch (c) {
  case CTRL('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 4);
    exit(EXIT_SUCCESS);
    break;
  case HOME_KEY:
    editor_config.cursor_x = 0;
    break;
  case END_KEY:
    editor_config.cursor_x = editor_config.screen_cols - 1;
    break;
  case PAGE_UP:
  case PAGE_DOWN:
    for (int i = 0; i < editor_config.screen_rows; i++)
      editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editor_move_cursor(c);
    break;
  }
}

static void editor_refresh_screen(void) {
  struct append_buffer ab;
  char buf[32];

  AB_INIT(&ab);

  AB_APPEND(&ab, "\x1b[?25l", 6);
  AB_APPEND(&ab, "\x1b[H", 3);

  editor_draw_rows(&ab);

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", editor_config.cursor_y + 1,
           editor_config.cursor_x + 1);
  AB_APPEND(&ab, buf, strlen(buf));

  AB_APPEND(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);

  AB_FREE(&ab);
}

static void editor_draw_rows(struct append_buffer *ab) {
  for (int y = 0; y < editor_config.screen_rows; y++) {
    if (y == editor_config.screen_rows / 3) {
      char welcome[80];
      int padding;
      int welcome_len = snprintf(welcome, sizeof(welcome),
                                 "Kilo editor -- version %s", KILO_VERSION);

      if (welcome_len > editor_config.screen_cols)
        welcome_len = editor_config.screen_cols;

      padding = (editor_config.screen_cols - welcome_len) / 2;
      if (padding != 0) {
        AB_APPEND(ab, "~", 1);
        padding--;
      }

      while ((padding--) != 0)
        AB_APPEND(ab, " ", 1);

      AB_APPEND(ab, welcome, welcome_len);
    } else
      AB_APPEND(ab, "~", 1);

    AB_APPEND(ab, "\x1b[K", 3);
    if (y < editor_config.screen_rows - 1)
      AB_APPEND(ab, "\r\n", 2);
  }
}

static void init_editor(void) {
  editor_config.cursor_x = editor_config.cursor_y = 0;
  if (get_window_size(&editor_config.screen_rows, &editor_config.screen_cols) ==
      -1)
    die("get_window_size");
}
