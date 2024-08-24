#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termio.h>
#include <termios.h>
#include <unistd.h>

#include "tvim.h"

#define CTRL_KEY(k) ((k) & 0x1F)

// Function Definitions:
void crash(const char*);
void clean_exit();

void terminal_disable_raw_mode();
void terminal_enable_raw_mode();
int terminal_get_window_size(size_t*, size_t*);
void terminal_refresh_screen();
void terminal_init();
int terminal_get_cursor_pos(size_t*, size_t*);

int tvim_read_key();
void tvim_draw_rows(struct abuf*);
void tvim_process_key(int c);

struct editorConfig tvimConfig;


/* EXIT */

void crash(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void clean_exit() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
}

/* APPEND BUFFER */

struct abuf ab_init() {
    // initial size 8, randomly selected can change.
    char* buffer = (char*)malloc(sizeof(char) * 8);
    size_t capacity = 8;
    size_t length = 0;

    struct abuf ab = {
        .buf = buffer,
        .capacity = capacity,
        .length = length,
    };

    return ab;
}

void ab_append(struct abuf* ab, const char* s, int len) {
    // Realloc memory, keep increasing until enough memory allocated.
    if (ab->length + len > ab->capacity) {
        size_t new_capacity = ab->capacity;
        while (ab->length + len > new_capacity) {
            new_capacity *= 2;
        }
        ab->buf = (char*)realloc(ab->buf, new_capacity * sizeof(char));
        if (ab->buf == NULL) {
            crash("Realloc");
        }
        ab->capacity = new_capacity;
    }

    // copy new string into buffer.
    memcpy(&ab->buf[ab->length], s, len);
    ab->length += len;
}

void ab_free(struct abuf* ab) {
    if (ab->buf != NULL) {
        free(ab->buf);
    }
}

/* Terminal */

void terminal_disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tvimConfig.original_termios) ==
        -1) {
        crash("tcsetattr");
    }
}

void terminal_enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &tvimConfig.original_termios) == -1) {
        crash("tcgetattr");
    }
    atexit(terminal_disable_raw_mode);

    struct termios raw = tvimConfig.original_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 3;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        crash("tcsetattr");
    }
}

int terminal_get_window_size(size_t* rows, size_t* cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return terminal_get_cursor_pos(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

int terminal_get_cursor_pos(size_t* rows, size_t* cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%zu;%zu", rows, cols) != 2)
        return -1;
    return 0;
}

void terminal_refresh_screen() {
    struct abuf ab = ab_init();
    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    tvim_draw_rows(&ab);

    char buf[70];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", tvimConfig.curY + 1,
             tvimConfig.curX + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.length);
    ab_free(&ab);
}

void terminal_init() {
    tvimConfig.curX = 0;
    tvimConfig.curY = 0;
    tvimConfig.tvimMode = NORMAL;
    tvimConfig.num_rows = 0;
    if (terminal_get_window_size(&tvimConfig.screenRows,
                                 &tvimConfig.screenCols) == -1) {
        crash("getWindowSize");
    }
}

/* FILE IO */



/* TVIM */

void tvim_draw_rows(struct abuf* ab) {
    for (size_t y = 0; y < tvimConfig.screenRows; y++) {
        if (y == tvimConfig.screenRows / 3) {
            char welcome[80];
            size_t welcomelen =
                snprintf(welcome, sizeof(welcome),
                         "T(errible)VIM editor -- version %s", TVIM_VERSION);
            if (welcomelen > tvimConfig.screenCols)
                welcomelen = tvimConfig.screenCols;
            int padding = (tvimConfig.screenCols - welcomelen) / 2;
            if (padding) {
                ab_append(ab, "~", 1);
                padding--;
            }
            while (padding--)
                ab_append(ab, " ", 1);
            ab_append(ab, welcome, welcomelen);
        } else {
            ab_append(ab, "~", 1);
        }

        ab_append(ab, "\x1b[K", 3);

        if (y < tvimConfig.screenRows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

int tvim_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            crash("Read");
        }
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[') {
            if (isdigit(seq[1])) {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }

                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '3':
                        return DELETE_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A':
                    return ARROW_UP;
                    break;
                case 'B':
                    return ARROW_DOWN;
                    break;
                case 'C':
                    return ARROW_RIGHT;
                    break;
                case 'D':
                    return ARROW_LEFT;
                }
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

void tvim_move_cursor(int key) {
    switch (key) {
    case ARROW_LEFT:
        if (tvimConfig.curX != 0) {
            tvimConfig.curX--;
        }
        break;
    case ARROW_DOWN:
        if (tvimConfig.curY != (int)tvimConfig.screenRows - 1) {
            tvimConfig.curY++;
        }
        break;
    case ARROW_RIGHT:
        if (tvimConfig.curX != (int)tvimConfig.screenCols - 1) {
            tvimConfig.curX++;
        }
        break;
    case ARROW_UP:
        if (tvimConfig.curY != 0) {
            tvimConfig.curY--;
        }
        break;
    }
}

void tvim_write_char(char c) {
    // TODO
}

void tvim_delete_char() {
    // TODO
}

void tvim_process_key(int c) {
    switch (tvimConfig.tvimMode) {
    case INSERT:
        switch (c) {
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            tvim_move_cursor(c);
            break;
        case DELETE_KEY:
            tvim_delete_char();
            tvimConfig.tvimMode = NORMAL;
            break;
        case CTRL_KEY('q'):
            clean_exit();
            break;
        default:
            tvim_write_char(c);
        }
        break;
    case NORMAL:
        switch (c) {
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            tvim_move_cursor(c);
            break;
        case DELETE_KEY:
            tvim_delete_char();
            tvimConfig.tvimMode = NORMAL;
            break;
        case CTRL_KEY('q'):
            clean_exit();
            break;
        default:
            break;
        }
    case VISUAL:
        switch (c) {
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            tvim_move_cursor(c);
            break;
        case DELETE_KEY:
            tvim_delete_char();
            tvimConfig.tvimMode = NORMAL;
            break;
        case CTRL_KEY('q'):
            clean_exit();
            break;
        }
        break;
    }
}

int main() {
    terminal_init();
    terminal_enable_raw_mode();
    while (1) {
        terminal_refresh_screen();
        int c = tvim_read_key();
        tvim_process_key(c);
    }
    return 0;
}
