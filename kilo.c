#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/queue.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

struct editor_row {
  int size, render_size;
  char *chars;
  char *render;
};

struct editor_config {
  int cursor_x, cursor_y;
  int render_x;
  int row_offset, col_offset;
  int screen_rows, screen_cols;
  int num_rows;
  char *file;
  char statusmsg[80];
  time_t statusmsg_time;
  struct editor_row *rows;
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

static int editor_row_cx_to_rx(struct editor_row *row, int cursor_x);
static void editor_update_row(struct editor_row *row);
static void editor_append_row(const char *s, size_t len);

static void editor_open(const char *file);
static void editor_move_cursor(int key);
static void editor_process_keypress(void);

static void editor_refresh_screen(void);
static void editor_set_status_message(const char *, ...);
static void editor_scroll(void);
static void editor_draw_status_bar(struct append_buffer *ab);
static void editor_draw_message_bar(struct append_buffer *ab);
static void editor_draw_rows(struct append_buffer *ab);

static void init_editor(void);

int main(int argc, char *argv[]) {
  enable_raw_mode();
  init_editor();

  if (argc >= 2)
    editor_open(argv[1]);

  editor_set_status_message("HELP: CTRL-Q = quit");

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

static int editor_row_cx_to_rx(struct editor_row *row, int cursor_x) {
  int render_x = 0;

  for (int i = 0; i < cursor_x; i++) {
    if (row->chars[i] == '\t')
      render_x += (KILO_TAB_STOP - 1) - (render_x % KILO_TAB_STOP);
    render_x++;
  }

  return (render_x);
}

static void editor_update_row(struct editor_row *row) {
  int i = 0, tabs = 0;

  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[i++] = ' ';
      while (i % KILO_TAB_STOP != 0)
        row->render[i++] = ' ';
    } else
      row->render[i++] = row->chars[j];
  }

  row->render[i] = '\0';
  row->render_size = i;
}

static void editor_append_row(const char *s, size_t len) {
  int n = editor_config.num_rows;

  editor_config.rows =
      realloc(editor_config.rows, sizeof(struct editor_config) * (n + 1));

  editor_config.rows[n].size = len;
  editor_config.rows[n].chars = malloc(len + 1);
  memcpy(editor_config.rows[n].chars, s, len);
  editor_config.rows[n].chars[len] = '\0';

  editor_config.rows[n].render_size = 0;
  editor_config.rows[n].render = NULL;
  editor_update_row(&editor_config.rows[n]);

  editor_config.num_rows++;
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

static void editor_open(const char *file) {
  size_t line_cap = 0;
  ssize_t line_len;
  char *line = NULL;
  FILE *fp = fopen(file, "r");

  if (fp == NULL)
    die("fopen");

  editor_config.file = strdup(file);

  while ((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 &&
           (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
      line_len--;
    editor_append_row(line, line_len);
  }

  free(line);
  fclose(fp);
}

static void editor_move_cursor(int key) {
  struct editor_row *row = editor_config.cursor_y >= editor_config.num_rows
                               ? NULL
                               : &editor_config.rows[editor_config.cursor_y];
  int row_len;

  switch (key) {
  case ARROW_LEFT:
    if (editor_config.cursor_x != 0)
      editor_config.cursor_x--;
    else if (editor_config.cursor_y > 0)
      editor_config.cursor_x =
          editor_config.rows[--editor_config.cursor_y].size;
    break;
  case ARROW_RIGHT:
    if (row != NULL) {
      if (editor_config.cursor_x < row->size)
        editor_config.cursor_x++;
      else if (editor_config.cursor_x == row->size) {
        editor_config.cursor_x = 0;
        editor_config.cursor_y++;
      }
    }
    break;
  case ARROW_UP:
    if (editor_config.cursor_y != 0)
      editor_config.cursor_y--;
    break;
  case ARROW_DOWN:
    if (editor_config.cursor_y < editor_config.num_rows)
      editor_config.cursor_y++;
    break;
  }

  row = editor_config.cursor_y >= editor_config.num_rows
            ? NULL
            : &editor_config.rows[editor_config.cursor_y];
  row_len = row == NULL ? 0 : row->size;
  if (editor_config.cursor_x > row_len)
    editor_config.cursor_x = row_len;
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
    if (editor_config.cursor_y < editor_config.num_rows)
      editor_config.cursor_x = editor_config.rows[editor_config.cursor_y].size;
    break;
  case PAGE_UP:
    editor_config.cursor_y = editor_config.row_offset;
    for (int i = 0; i < editor_config.screen_rows; i++)
      editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    break;
  case PAGE_DOWN:
    editor_config.cursor_y =
        editor_config.row_offset + editor_config.screen_rows - 1;
    if (editor_config.cursor_y > editor_config.num_rows)
      editor_config.cursor_y = editor_config.num_rows;

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

  editor_scroll();

  AB_INIT(&ab);
  AB_APPEND(&ab, "\x1b[?25l", 6);
  AB_APPEND(&ab, "\x1b[H", 3);

  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);
  editor_draw_message_bar(&ab);

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
           editor_config.cursor_y - editor_config.row_offset + 1,
           editor_config.render_x - editor_config.col_offset + 1);
  AB_APPEND(&ab, buf, strlen(buf));

  AB_APPEND(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);

  AB_FREE(&ab);
}

static void editor_set_status_message(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(editor_config.statusmsg, sizeof(editor_config.statusmsg), fmt, ap);
  va_end(ap);

  editor_config.statusmsg_time = time(NULL);
}

static void editor_scroll(void) {
  editor_config.render_x = 0;
  if (editor_config.cursor_y < editor_config.num_rows)
    editor_config.render_x = editor_row_cx_to_rx(
        &editor_config.rows[editor_config.cursor_y], editor_config.cursor_x);

  if (editor_config.cursor_y < editor_config.row_offset)
    editor_config.row_offset = editor_config.cursor_y;

  if (editor_config.cursor_y >=
      editor_config.row_offset + editor_config.screen_rows)
    editor_config.row_offset =
        editor_config.cursor_y - editor_config.screen_rows + 1;

  if (editor_config.render_x < editor_config.col_offset)
    editor_config.col_offset = editor_config.render_x;

  if (editor_config.render_x >=
      editor_config.col_offset + editor_config.screen_cols)
    editor_config.col_offset =
        editor_config.render_x - editor_config.screen_cols + 1;
}

static void editor_draw_status_bar(struct append_buffer *ab) {
  char status[80], status_right[80];
  int len =
      snprintf(status, sizeof(status), "%.20s - %d lines",
               editor_config.file == NULL ? "[No Name]" : editor_config.file,
               editor_config.num_rows);
  int rlen = snprintf(status_right, sizeof(status_right), "%d/%d",
                      editor_config.cursor_y + 1, editor_config.num_rows);

  if (len > editor_config.screen_cols)
    len = editor_config.screen_cols;

  AB_APPEND(ab, "\x1b[7m", 4);
  AB_APPEND(ab, status, len);

  while (len < editor_config.screen_cols) {
    if (editor_config.screen_cols - len == rlen) {
      AB_APPEND(ab, status_right, rlen);
      break;
    }

    AB_APPEND(ab, " ", 1);
    len++;
  }

  AB_APPEND(ab, "\x1b[m", 3);
  AB_APPEND(ab, "\r\n", 3);
}

static void editor_draw_message_bar(struct append_buffer *ab) {
  int msg_len = strlen(editor_config.statusmsg);

  AB_APPEND(ab, "\x1b[K", 3);

  if (msg_len > editor_config.screen_cols)
    msg_len = editor_config.screen_cols;

  if (msg_len != 0 && time(NULL) - editor_config.statusmsg_time < 5)
    AB_APPEND(ab, editor_config.statusmsg, msg_len);
}

static void editor_draw_rows(struct append_buffer *ab) {
  for (int y = 0; y < editor_config.screen_rows; y++) {
    int file_row = y + editor_config.row_offset;
    if (file_row >= editor_config.num_rows) {
      if (editor_config.num_rows == 0 && y == editor_config.screen_rows / 3) {
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
    } else {
      int len =
          editor_config.rows[file_row].render_size - editor_config.col_offset;

      if (len < 0)
        len = 0;

      if (len > editor_config.screen_cols)
        len = editor_config.screen_cols;

      AB_APPEND(ab,
                &editor_config.rows[file_row].render[editor_config.col_offset],
                len);
    }

    AB_APPEND(ab, "\x1b[K", 3);
    AB_APPEND(ab, "\r\n", 2);
  }
}

static void init_editor(void) {
  editor_config.cursor_x = editor_config.cursor_y = 0;
  editor_config.render_x = 0;
  editor_config.row_offset = editor_config.col_offset = 0;
  editor_config.num_rows = 0;
  editor_config.rows = NULL;
  editor_config.file = NULL;
  editor_config.statusmsg[0] = '\0';
  editor_config.statusmsg_time = 0;

  if (get_window_size(&editor_config.screen_rows, &editor_config.screen_cols) ==
      -1)
    die("get_window_size");

  editor_config.screen_rows -= 2;
}
