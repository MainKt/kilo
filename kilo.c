#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioccom.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/sbuf.h>
#include <sys/stat.h>
#include <sys/ttycom.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

struct editor_syntax {
  char *file_type;
  char **file_match;
  char **keywords;
  char *single_line_comment_start;
  char *multi_line_comment_start, *multi_line_comment_end;
  int flags;
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

enum editor_highlight : char {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

struct editor_row {
  int i, size, render_size;
  bool hl_open_comment;
  char *chars, *render;
  enum editor_highlight *highlight;
};

struct editor_config {
  int cursor_x, cursor_y;
  int render_x;
  int row_offset, col_offset;
  int screen_rows, screen_cols;
  int num_rows;
  int dirty;
  char *file;
  char statusmsg[80];
  time_t statusmsg_time;
  struct editor_syntax *syntax;
  struct editor_row *rows;
  struct termios orignal_termios;
};

static struct editor_config editor;

static char *c_hl_extensions[] = {".c", ".h", ".cpp", NULL};
static char *c_hl_keywords[] = {
    "switch", "if",    "while",     "for",     "break",   "continue",
    "return", "else",  "struct",    "union",   "typedef", "static",
    "enum",   "class", "case",      "int|",    "long|",   "double|",
    "float|", "char|", "unsigned|", "signed|", "void|",   NULL};

static struct editor_syntax highlight_db[] = {
    {
        "c",
        c_hl_extensions,
        c_hl_keywords,
        "//",
        "/*",
        "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS,
    },
};

enum editor_key {
  BACKSPACE = 127,
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

static void die(const char *);
static void enable_raw_mode(void);
static void disable_raw_mode(void);
static int editor_read_key(void);
static int get_cursor_position(int *rows, int *cols);
static int get_window_size(int *rows, int *cols);

static bool is_separator(char c);
static void editor_update_syntax(struct editor_row *row);
static int editor_syntax_to_color(enum editor_highlight hl);
static void editor_select_syntax_highlight(void);

static int editor_row_cx_to_rx(struct editor_row *row, int cursor_x);
static int editor_row_rx_to_cx(struct editor_row *row, int render_x);
static void editor_update_row(struct editor_row *row);
static void editor_insert_row(int at, const char *s, size_t len);
static void editor_free_row(struct editor_row *row);
static void editor_del_row(int at);
static void editor_row_append_string(struct editor_row *, const char *, size_t);
static void editor_row_insert_char(struct editor_row *row, int at, char c);
static void editor_row_del_char(struct editor_row *row, int at);

static void editor_insert_char(char c);
static void editor_insert_newline(void);
static void editor_del_char(void);

static char *editor_rows_to_string(int *buflen);
static void editor_open(const char *file);
static void editor_save(void);
static void editor_find(void);
static void editor_find_callback(const char *, int);
static char *editor_prompt(const char *, void (*)(const char *, int));
static void editor_move_cursor(int key);
static void editor_process_keypress(void);

static void editor_refresh_screen(void);
static void editor_set_status_message(const char *, ...);
static void editor_scroll(void);
static void editor_draw_status_bar(struct sbuf *sb);
static void editor_draw_message_bar(struct sbuf *sb);
static void editor_draw_rows(struct sbuf *sb);

static void init_editor(void);

int main(int argc, char *argv[]) {
  enable_raw_mode();
  init_editor();

  if (argc > 1)
    editor_open(argv[1]);

  editor_set_status_message(
      "HELP: CTRL-S = save | CTRL-Q = quit | CTRL-F = find");

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
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.orignal_termios) == -1)
    die("tcsetattr");
}

static void enable_raw_mode(void) {
  struct termios raw;

  if (tcgetattr(STDIN_FILENO, &editor.orignal_termios) == -1)
    die("tcgetattr");

  atexit(disable_raw_mode);

  raw = editor.orignal_termios;
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

static bool is_separator(char c) {
  return (isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL);
}

static void editor_update_syntax(struct editor_row *row) {
  int i = 0, slcs_len = 0, mlcs_len = 0, mlce_len = 0;
  bool prev_sep = true, in_comment = false;
  char quote = '\0';
  char *slcs = NULL, *mlcs = NULL, *mlce = NULL;

  row->highlight = realloc(row->highlight, row->render_size);
  memset(row->highlight, HL_NORMAL, row->render_size);

  if (editor.syntax == NULL)
    return;

  slcs = editor.syntax->single_line_comment_start;
  mlcs = editor.syntax->multi_line_comment_start;
  mlce = editor.syntax->multi_line_comment_end;

  slcs_len = slcs == NULL ? 0 : strlen(slcs);
  mlcs_len = mlcs == NULL ? 0 : strlen(mlcs);
  mlce_len = mlce == NULL ? 0 : strlen(mlce);

  in_comment = (row->i > 0 && editor.rows[row->i - 1].hl_open_comment);

  while (i < row->render_size) {
    char c = row->render[i];
    enum editor_highlight prev_hl = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

    if (slcs_len != 0 && quote == '\0' && !in_comment &&
        strncmp(&row->render[i], slcs, slcs_len) == 0) {
      memset(&row->highlight[i], HL_COMMENT, row->render_size - i);
      break;
    }

    if (mlcs_len != 0 && mlce_len != 0 && quote == '\0') {
      if (in_comment) {
        row->highlight[i] = HL_MLCOMMENT;
        if (strncmp(&row->render[i], mlce, mlce_len) == 0) {
          memset(&row->highlight[i], HL_MLCOMMENT, mlce_len);
          i += mlce_len;
          in_comment = false;
          prev_sep = true;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (strncmp(&row->render[i], mlcs, mlcs_len) == 0) {
        memset(&row->highlight[i], HL_MLCOMMENT, mlcs_len);
        i += mlcs_len;
        in_comment = true;
        continue;
      }
    }

    if (editor.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (quote != '\0') {
        row->highlight[i] = HL_STRING;

        if (c == '\\' && i + 1 < row->render_size) {
          row->highlight[i + 1] = HL_STRING;
          i += 2;
          continue;
        }

        if (c == quote)
          quote = '\0';

        i++;
        prev_sep = true;
        continue;
      } else if (c == '"' || c == '\'') {
        quote = c;
        row->highlight[i] = HL_STRING;
        i++;
        continue;
      }
    }

    if (editor.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if (isdigit(c) && ((prev_sep || prev_hl == HL_NUMBER) ||
                         (c == '.' && prev_hl == HL_NUMBER))) {
        row->highlight[i] = HL_NUMBER;
        i++;
        prev_sep = false;
        continue;
      }
    }

    if (prev_sep && editor.syntax->keywords != NULL) {
      char **keywords = editor.syntax->keywords;
      int j = 0;

      while (keywords[j] != NULL) {
        int keyword_len = strlen(keywords[j]);
        int is_keyword2 = keywords[j][keyword_len - 1] == '|';

        if (is_keyword2)
          keyword_len--;

        if (strncmp(&row->render[i], keywords[j], keyword_len) == 0 &&
            is_separator(row->render[i + keyword_len])) {
          memset(&row->highlight[i], is_keyword2 ? HL_KEYWORD2 : HL_KEYWORD1,
                 keyword_len);
          i += keyword_len;
          break;
        }
        j++;
      }

      if (keywords[j] != NULL) {
        prev_sep = false;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }

  bool changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if (changed && row->i + 1 < editor.num_rows)
    editor_update_row(&editor.rows[row->i + 1]);
}

static int editor_syntax_to_color(enum editor_highlight hl) {
  switch (hl) {
  case HL_COMMENT:
  case HL_MLCOMMENT:
    return (36);
  case HL_STRING:
    return (35);
  case HL_KEYWORD1:
    return (33);
  case HL_KEYWORD2:
    return (32);
  case HL_NUMBER:
    return (31);
  case HL_MATCH:
    return (34);
  default:
    return (37);
  }
}

static void editor_select_syntax_highlight(void) {
  char *extension = NULL;

  editor.syntax = NULL;
  if (editor.file == NULL)
    return;

  extension = strrchr(editor.file, '.');

  for (size_t i = 0; i < nitems(highlight_db); i++) {
    struct editor_syntax *s = &highlight_db[i];

    for (size_t j = 0; s->file_match[j] != NULL; j++) {
      bool is_extension = (s->file_match[j][0] == '.');

      if ((is_extension && extension != NULL &&
           strcmp(extension, s->file_match[i]) == 0) ||
          (!is_extension && strstr(editor.file, s->file_match[i]) != NULL)) {
        editor.syntax = s;
        for (int file_row = 0; file_row < editor.num_rows; file_row++)
          editor_update_syntax(&editor.rows[file_row]);
        return;
      }
    }
  }
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

static int editor_row_rx_to_cx(struct editor_row *row, int render_x) {
  int cursor_x = 0, current_render_x = 0;

  while (cursor_x < row->size) {
    if (row->chars[cursor_x] == '\t')
      current_render_x +=
          (KILO_TAB_STOP - 1) - (current_render_x % KILO_TAB_STOP);
    current_render_x++;

    if (current_render_x > render_x)
      return (cursor_x);

    cursor_x++;
  }

  return (cursor_x);
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

  editor_update_syntax(row);
}

static void editor_insert_row(int at, const char *s, size_t len) {
  if (at < 0 || at > editor.num_rows)
    return;

  editor.rows =
      realloc(editor.rows, sizeof(struct editor_row) * (editor.num_rows + 1));
  memmove(&editor.rows[at + 1], &editor.rows[at],
          sizeof(struct editor_row) * (editor.num_rows - at));
  for (int i = at + 1; i <= editor.num_rows; i++)
    editor.rows[i].i++;

  editor.rows[at].i = at;

  editor.rows[at].size = len;
  editor.rows[at].chars = malloc(len + 1);
  memcpy(editor.rows[at].chars, s, len);
  editor.rows[at].chars[len] = '\0';

  editor.rows[at].render_size = 0;
  editor.rows[at].render = NULL;
  editor.rows[at].highlight = NULL;
  editor.rows[at].hl_open_comment = false;
  editor_update_row(&editor.rows[at]);

  editor.num_rows++;
  editor.dirty++;
}

static void editor_free_row(struct editor_row *row) {
  free(row->render);
  free(row->chars);
  free(row->highlight);
}

static void editor_del_row(int at) {
  if (at < 0 || at >= editor.num_rows)
    return;

  editor_free_row(&editor.rows[at]);
  memmove(&editor.rows[at], &editor.rows[at + 1],
          sizeof(struct editor_row) * (editor.num_rows - at - 1));

  for (int i = at; i < editor.num_rows - 1; i++)
    editor.rows[i].i--;

  editor.num_rows--;
  editor.dirty++;
}

static void editor_row_append_string(struct editor_row *row, const char *s,
                                     size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';

  editor_update_row(row);
  editor.dirty++;
}

static void editor_row_insert_char(struct editor_row *row, int at, char c) {
  if (at < 0 || at > row->size)
    at = row->size;

  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editor_update_row(row);
  editor.dirty++;
}

static void editor_row_del_char(struct editor_row *row, int at) {
  if (at < 0 || at >= row->size)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editor_update_row(row);
  editor.dirty++;
}

static void editor_insert_char(char c) {
  if (editor.cursor_y == editor.num_rows)
    editor_insert_row(editor.num_rows, "", 0);

  editor_row_insert_char(&editor.rows[editor.cursor_y], editor.cursor_x++, c);
}

static void editor_insert_newline(void) {
  if (editor.cursor_x == 0) {
    editor_insert_row(editor.cursor_y, "", 0);
  } else {
    struct editor_row *row = &editor.rows[editor.cursor_y];

    editor_insert_row(editor.cursor_y + 1, &row->chars[editor.cursor_x],
                      row->size - editor.cursor_x);
    row = &editor.rows[editor.cursor_y];
    row->size = editor.cursor_x;
    row->chars[row->size] = '\0';
    editor_update_row(row);
  }
  editor.cursor_y++;
  editor.cursor_x = 0;
}

static void editor_del_char(void) {
  if (editor.cursor_y == editor.num_rows)
    return;

  if (editor.cursor_x == 0 && editor.cursor_y == 0)
    return;

  if (editor.cursor_x > 0) {
    editor_row_del_char(&editor.rows[editor.cursor_y], editor.cursor_x - 1);
    editor.cursor_x--;
  } else {
    editor.cursor_x = editor.rows[editor.cursor_y - 1].size;
    editor_row_append_string(&editor.rows[editor.cursor_y - 1],
                             editor.rows[editor.cursor_y].chars,
                             editor.rows[editor.cursor_y].size);
    editor_del_row(editor.cursor_y);
    editor.cursor_y--;
  }
}

static char *editor_rows_to_string(int *buflen) {
  int total_len = 0;
  char *buf;
  char *p;

  for (int i = 0; i < editor.num_rows; i++)
    total_len += editor.rows[i].size + 1;

  *buflen = total_len;
  buf = malloc(total_len);
  p = buf;
  for (int i = 0; i < editor.num_rows; i++) {
    memcpy(p, editor.rows[i].chars, editor.rows[i].size);
    p += editor.rows[i].size;
    *p++ = '\n';
  }

  return (buf);
}

static void editor_open(const char *file) {
  size_t line_cap = 0;
  ssize_t line_len;
  char *line = NULL;
  FILE *fp = fopen(file, "r");

  if (fp == NULL)
    die("fopen");

  editor.file = strdup(file);

  editor_select_syntax_highlight();

  while ((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 &&
           (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
      line_len--;
    editor_insert_row(editor.num_rows, line, line_len);
  }

  free(line);
  fclose(fp);
  editor.dirty = 0;
}

static void editor_save(void) {
  int len;
  int fd = -1;
  char *buf = editor_rows_to_string(&len);

  if (editor.file == NULL) {
    editor.file = editor_prompt("Save as: %s", NULL);

    if (editor.file == NULL) {
      editor_set_status_message("Save aborted");
      return;
    }

    editor_select_syntax_highlight();
  }

  fd = open(editor.file, O_RDWR | O_CREAT,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd == -1) {
    editor_set_status_message("Can't save! I/O error %s", strerror(errno));
    goto cleanup;
  }

  if (ftruncate(fd, len) == -1) {
    editor_set_status_message("Can't save! I/O error %s", strerror(errno));
    goto cleanup;
  }

  if (write(fd, buf, len) != len) {
    editor_set_status_message("Can't save! I/O error %s", strerror(errno));
    goto cleanup;
  }

  editor_set_status_message("%d bytes written to disk", len);
  editor.dirty = 0;

cleanup:
  close(fd);
  free(buf);
}

static void editor_find(void) {
  int save_cursor_x = editor.cursor_x;
  int save_cursor_y = editor.cursor_y;
  int saved_col_offset = editor.col_offset;
  int saved_row_offset = editor.row_offset;

  char *query =
      editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);

  if (query == NULL) {
    editor.cursor_x = save_cursor_x;
    editor.cursor_y = save_cursor_y;
    editor.col_offset = saved_col_offset;
    editor.row_offset = saved_row_offset;
  } else
    free(query);
}

static void editor_find_callback(const char *query, int key) {
  static int last_match = -1;
  static int direction = 1;
  static int saved_hl_line;
  static char *saved_hl = NULL;

  if (saved_hl != NULL) {
    memcpy(editor.rows[saved_hl_line].highlight, saved_hl,
           editor.rows[saved_hl_line].render_size);
    free(saved_hl);
    saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1)
    direction = 1;

  for (int i = 0, current = last_match + direction; i < editor.num_rows;
       i++, current += direction) {
    struct editor_row *row;
    char *match;

    if (current == -1)
      current = editor.num_rows - 1;

    if (current == editor.num_rows)
      current = 0;

    row = &editor.rows[current];
    match = strstr(row->render, query);

    if (match != NULL) {
      int rx = match - row->render;
      last_match = current;
      editor.cursor_y = current;
      editor.cursor_x = editor_row_rx_to_cx(row, rx);
      editor.row_offset = editor.num_rows;

      saved_hl_line = current;
      saved_hl = malloc(row->render_size);
      memcpy(saved_hl, row->highlight, row->render_size);

      memset(&row->highlight[rx], HL_MATCH, strlen(query));
      break;
    }
  }
}

static char *editor_prompt(const char *prompt,
                           void (*callback)(const char *, int)) {
  size_t size = 128, len = 0;
  char *buf = malloc(size);

  buf[0] = '\0';

  for (;;) {
    int c;

    editor_set_status_message(prompt, buf);
    editor_refresh_screen();

    c = editor_read_key();
    if ((c == DEL_KEY || c == CTRL('h') || c == BACKSPACE) && len != 0) {
      buf[--len] = '\0';
    } else if (c == '\x1b') {
      editor_set_status_message("");
      if (callback != NULL)
        callback(buf, c);
      free(buf);
      return (NULL);
    } else if (c == '\r' && len != 0) {
      editor_set_status_message("");
      if (callback != NULL)
        callback(buf, c);
      return (buf);
    } else if (!iscntrl(c) && c < 128) {
      if (len == size - 1) {
        size *= 2;
        buf = realloc(buf, size);
      }
      buf[len++] = c;
      buf[len] = '\0';
    }

    if (callback != NULL)
      callback(buf, c);
  }
}

static void editor_move_cursor(int key) {
  struct editor_row *row =
      editor.cursor_y >= editor.num_rows ? NULL : &editor.rows[editor.cursor_y];
  int row_len;

  switch (key) {
  case ARROW_LEFT:
    if (editor.cursor_x != 0)
      editor.cursor_x--;
    else if (editor.cursor_y > 0)
      editor.cursor_x = editor.rows[--editor.cursor_y].size;
    break;
  case ARROW_RIGHT:
    if (row != NULL) {
      if (editor.cursor_x < row->size)
        editor.cursor_x++;
      else if (editor.cursor_x == row->size) {
        editor.cursor_x = 0;
        editor.cursor_y++;
      }
    }
    break;
  case ARROW_UP:
    if (editor.cursor_y != 0)
      editor.cursor_y--;
    break;
  case ARROW_DOWN:
    if (editor.cursor_y < editor.num_rows)
      editor.cursor_y++;
    break;
  }

  row =
      editor.cursor_y >= editor.num_rows ? NULL : &editor.rows[editor.cursor_y];
  row_len = row == NULL ? 0 : row->size;
  if (editor.cursor_x > row_len)
    editor.cursor_x = row_len;
}

static void editor_process_keypress(void) {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editor_read_key();

  switch (c) {
  case '\r':
    editor_insert_newline();
    break;
  case CTRL('q'):
    if (editor.dirty != 0 && quit_times > 0) {
      editor_set_status_message("WARNING!!! File has unsaved changes. "
                                "Press Ctrl-Q %d more times to quit.",
                                quit_times--);
      return;
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 4);
    exit(EXIT_SUCCESS);
    break;
  case CTRL('s'):
    editor_save();
    break;
  case HOME_KEY:
    editor.cursor_x = 0;
    break;
  case END_KEY:
    if (editor.cursor_y < editor.num_rows)
      editor.cursor_x = editor.rows[editor.cursor_y].size;
    break;
  case CTRL('f'):
    editor_find();
    break;
  case BACKSPACE:
  case CTRL('h'):
  case DEL_KEY:
    if (c == DEL_KEY)
      editor_move_cursor(ARROW_RIGHT);
    editor_del_char();
    break;
  case PAGE_UP:
    editor.cursor_y = editor.row_offset;
    for (int i = 0; i < editor.screen_rows; i++)
      editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    break;
  case PAGE_DOWN:
    editor.cursor_y = editor.row_offset + editor.screen_rows - 1;
    if (editor.cursor_y > editor.num_rows)
      editor.cursor_y = editor.num_rows;

    for (int i = 0; i < editor.screen_rows; i++)
      editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editor_move_cursor(c);
    break;
  case CTRL('l'):
  case '\x1b':
    break;
  default:
    editor_insert_char(c);
    break;
  }

  quit_times = KILO_QUIT_TIMES;
}

static void editor_refresh_screen(void) {
  struct sbuf *sb = sbuf_new_auto();

  editor_scroll();

  sbuf_cat(sb, "\x1b[?25l");
  sbuf_cat(sb, "\x1b[H");

  editor_draw_rows(sb);
  editor_draw_status_bar(sb);
  editor_draw_message_bar(sb);

  sbuf_printf(sb, "\x1b[%d;%dH", editor.cursor_y - editor.row_offset + 1,
              editor.render_x - editor.col_offset + 1);

  sbuf_cat(sb, "\x1b[?25h");

  write(STDOUT_FILENO, sbuf_data(sb), sbuf_len(sb));

  sbuf_delete(sb);
}

static void editor_set_status_message(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(editor.statusmsg, sizeof(editor.statusmsg), fmt, ap);
  va_end(ap);

  editor.statusmsg_time = time(NULL);
}

static void editor_scroll(void) {
  editor.render_x = 0;
  if (editor.cursor_y < editor.num_rows)
    editor.render_x =
        editor_row_cx_to_rx(&editor.rows[editor.cursor_y], editor.cursor_x);

  if (editor.cursor_y < editor.row_offset)
    editor.row_offset = editor.cursor_y;

  if (editor.cursor_y >= editor.row_offset + editor.screen_rows)
    editor.row_offset = editor.cursor_y - editor.screen_rows + 1;

  if (editor.render_x < editor.col_offset)
    editor.col_offset = editor.render_x;

  if (editor.render_x >= editor.col_offset + editor.screen_cols)
    editor.col_offset = editor.render_x - editor.screen_cols + 1;
}

static void editor_draw_status_bar(struct sbuf *sb) {
  char status[80], status_right[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     editor.file == NULL ? "[No Name]" : editor.file,
                     editor.num_rows, editor.dirty != 0 ? "(modified)" : "");
  int rlen =
      snprintf(status_right, sizeof(status_right), "%s | %d/%d",
               editor.syntax == NULL ? "no ft" : editor.syntax->file_type,
               editor.cursor_y + 1, editor.num_rows);

  if (len > editor.screen_cols)
    len = editor.screen_cols;

  sbuf_cat(sb, "\x1b[7m");
  sbuf_bcat(sb, status, len);

  while (len < editor.screen_cols) {
    if (editor.screen_cols - len == rlen) {
      sbuf_bcat(sb, status_right, rlen);
      break;
    }

    sbuf_cat(sb, " ");
    len++;
  }

  sbuf_cat(sb, "\x1b[m");
  sbuf_cat(sb, "\r\n");
}

static void editor_draw_message_bar(struct sbuf *sb) {
  int msg_len = strlen(editor.statusmsg);

  sbuf_cat(sb, "\x1b[K");

  if (msg_len > editor.screen_cols)
    msg_len = editor.screen_cols;

  if (msg_len != 0 && time(NULL) - editor.statusmsg_time < 5)
    sbuf_bcat(sb, editor.statusmsg, msg_len);
}

static void editor_draw_rows(struct sbuf *sb) {
  for (int y = 0; y < editor.screen_rows; y++) {
    int file_row = y + editor.row_offset;
    if (file_row >= editor.num_rows) {
      if (editor.num_rows == 0 && y == editor.screen_rows / 3) {
        char welcome[80];
        int padding;
        int welcome_len = snprintf(welcome, sizeof(welcome),
                                   "Kilo editor -- version %s", KILO_VERSION);

        if (welcome_len > editor.screen_cols)
          welcome_len = editor.screen_cols;

        padding = (editor.screen_cols - welcome_len) / 2;
        if (padding != 0) {
          sbuf_cat(sb, "~");
          padding--;
        }

        while ((padding--) != 0)
          sbuf_cat(sb, " ");

        sbuf_bcat(sb, welcome, welcome_len);
      } else
        sbuf_cat(sb, "~");
    } else {
      int len = editor.rows[file_row].render_size - editor.col_offset;
      char *c;
      enum editor_highlight *hl;

      if (len < 0)
        len = 0;

      if (len > editor.screen_cols)
        len = editor.screen_cols;

      c = &editor.rows[file_row].render[editor.col_offset];
      hl = &editor.rows[file_row].highlight[editor.col_offset];

      for (int i = 0, current_color = -1; i < len; i++) {
        if (iscntrl(c[i])) {
          char symbol = (c[i] <= 26) ? '@' + c[i] : '?';

          sbuf_cat(sb, "\x1b[7m");
          sbuf_bcat(sb, &symbol, 1);
          sbuf_cat(sb, "\x1b[m");

          if (current_color != -1) {
            char color_buf[16];
            int color_len = snprintf(color_buf, sizeof(color_buf), "\x1b[%dm",
                                     current_color);

            sbuf_bcat(sb, color_buf, color_len);
          }
        } else if (hl[i] == HL_NORMAL) {
          if (current_color != -1) {
            sbuf_cat(sb, "\x1b[39m");
            current_color = -1;
          }
          sbuf_bcat(sb, &c[i], 1);
        } else {
          int color = editor_syntax_to_color(hl[i]);
          if (current_color != color) {
            current_color = color;
            char color_buf[16];
            int color_len =
                snprintf(color_buf, sizeof(color_buf), "\x1b[%dm", color);
            sbuf_bcat(sb, color_buf, color_len);
          }
          sbuf_bcat(sb, &c[i], 1);
        }
      }
      sbuf_cat(sb, "\x1b[39m");
    }

    sbuf_cat(sb, "\x1b[K");
    sbuf_cat(sb, "\r\n");
  }
}

static void init_editor(void) {
  editor.cursor_x = editor.cursor_y = 0;
  editor.render_x = 0;
  editor.row_offset = editor.col_offset = 0;
  editor.num_rows = 0;
  editor.dirty = 0;
  editor.rows = NULL;
  editor.file = NULL;
  editor.statusmsg[0] = '\0';
  editor.statusmsg_time = 0;
  editor.syntax = NULL;

  if (get_window_size(&editor.screen_rows, &editor.screen_cols) == -1)
    die("get_window_size");

  editor.screen_rows -= 2;
}
