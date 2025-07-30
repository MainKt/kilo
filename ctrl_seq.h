/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025, Muhammad Saheed <saheed@FreeBSD.org>
 */

#ifndef CTRL_SEQ_H
#define CTRL_SEQ_H

#define STR(x) #x
#define XSTR(x) STR(x)

#define ESC "\x1B"
#define ESC_CHAR '\x1B'
#define CSI ESC "["

#define DEC_SET(x) CSI "?" XSTR(x) "h"
#define DEC_RESET(x) CSI "?" XSTR(x) "l"

#define CURSOR_SHOW DEC_SET(25)
#define CURSOR_HIDE DEC_RESET(25)

#define ALT_BUF_ON DEC_SET(1049)
#define ALT_BUF_OFF DEC_RESET(1049)

#define CURSOR_UP(x) CSI XSTR(x) "A"
#define CURSOR_DOWN(x) CSI XSTR(x) "B"
#define CURSOR_FORWARD(x) CSI XSTR(x) "C"
#define CURSOR_BACK(x) CSI XSTR(x) "D"

#define SAVE_CURSOR_POS CSI "s"
#define RESTORE_CURSOR_POS CSI "u"

#define STATUS_REPORT(x) CSI XSTR(x) "n"
#define CURSOR_POS STATUS_REPORT(6)

#define CURSOR_MOVE(x, y) CSI XSTR(x) ";" XSTR(y) "H"
#define CURSOR_MOVE_FMT                                                        \
  CSI "%d"                                                                     \
      ";"                                                                      \
      "%d"                                                                     \
      "H"

#define ERASE_IN_DISPLAY(x) CSI XSTR(x) "J"
#define ERASE_IN_LINE(x) CSI XSTR(x) "K"
#define ERASE_TO_END 0
#define ERASE_TO_BEGINNING 1
#define ERASE_ENTIRE 2

#define SGR(x) CSI XSTR(x) "m"
#define SGR_FMT CSI "%dm"
#define RESET_SGR SGR(0)
#define BOLD SGR(1)
#define INVERT SGR(7)

#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGENTA 5
#define CYAN 6
#define WHITE 7

#define FG 3
#define BG 4
#define FG_BRIGHT 9
#define BG_BRIGHT 10

#define CAT(A, B) A##B
#define COLOR(type, color) SGR(CAT(type, color))
#define DEFAULT_FG COLOR(FG, 9)
#define DEFAULT_BG COLOR(BG, 9)

#endif /* !CTRL_SEQ_H */
