#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

/*** includes ***/
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termio.h>
#include <termios.h>
#include <unistd.h>

#include "tvim.h"

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1F)

#define return_defer(value) \
    do {                    \
        result = (value);   \
        goto defer;         \
    } while (0)

/*** data ***/

struct editorConfig tvimConfig;

/*** Exit ***/

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

void usage_error() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    fprintf(stderr, "Usage: tvim file");
    exit(1);
}

/*** terminal ***/

void terminal_disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tvimConfig.og_termios) == -1)
        crash("tcsetattr");
}

void terminal_enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &tvimConfig.og_termios) == -1)
        crash("tcgetattr");
    atexit(terminal_disable_raw_mode);

    struct termios raw = tvimConfig.og_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        crash("tcsetattr");
}

int terminal_get_cursor_position(int* rows, int* cols) {
    char buf[32];
    volatile unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }

        if (buf[i] == 'R') {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }

    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return 0;
}

int terminal_get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return terminal_get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int row_cX_to_rX(row_t* row, int cX) {
    int rX = 0;
    int j;
    for (j = 0; j < cX; j++) {
        if (row->chars[j] == '\t')
            rX += (TVIM_TAB_STOP - 1) - (rX % TVIM_TAB_STOP);
        rX++;
    }
    return rX;
}

void editorUpdateRow(row_t* row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->len; j++)
        if (row->chars[j] == '\t')
            tabs++;

    row->render = malloc(row->len + tabs * (TVIM_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->len; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TVIM_TAB_STOP != 0)
                row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rlen = idx;
}

void row_append(char* s, size_t len, int* linesRead) {
    int at = *linesRead;
    tvimConfig.rows[at].len = len;
    tvimConfig.rows[at].chars = malloc(len + 1);
    memcpy(tvimConfig.rows[at].chars, s, len);
    tvimConfig.rows[at].chars[len] = '\0';

    tvimConfig.rows[at].rlen = 0;
    tvimConfig.rows[at].render = NULL;
    editorUpdateRow(&tvimConfig.rows[at]);

    (*linesRead)++;
}

/*** file i/o ***/

int file_get_lines(FILE* file_ptr) {
    int lines = 0;
    while (!feof(file_ptr)) {
        char c = fgetc(file_ptr);
        if (ferror(file_ptr)) {
            return -1;
        }
        if (c == '\n') {
            lines++;
        }
    }
    if (fseek(file_ptr, 0, SEEK_SET) < 0) {
        return -1;
    }
    return lines;
}

void file_open(char* filename) {
    bool result = true;

    tvimConfig.filename = strdup(filename);

    FILE* fp = fopen(filename, "r");
    if (!fp)
        crash("fopen");

    int nLines = file_get_lines(fp);
    if (nLines == -1) {
        return_defer(false);
    }

    tvimConfig.rows = (row_t*)malloc(sizeof(row_t) * nLines + 1);
    if (tvimConfig.rows == NULL) {
        return_defer(false);
    }
    tvimConfig.nRows = nLines;

    int linesRead = 0;
    int nChars = 0;
    char c;
    while ((c = fgetc(fp)) != EOF) {
        if (ferror(fp)) {
            return_defer(false);
        }

        if (c == '\n' || c == '\r') {
            char* cRow = (char*)malloc((sizeof(char) * nChars) + 1);
            if (cRow == NULL) {
                printf("malloc\n");
                return_defer(false);
            }

            if (fseek(fp, -(nChars + 1), SEEK_CUR) < 0) {
                printf("fseek");
                return_defer(false);
            }

            fread(cRow, (nChars + 1) * sizeof(char), 1, fp);

            cRow[nChars + 1] = '\0';
            row_append(cRow, nChars, &linesRead);
            if (cRow != NULL)
                free(cRow);
            nChars = 0;
            continue;
        }

        nChars++;
    }

    if (nChars != 0) {
        char* cRow = (char*)malloc((sizeof(char) * nChars) + 1);
        if (cRow == NULL) {
            return_defer(false);
        }

        if (fseek(fp, -(nChars), SEEK_CUR) < 0) {
            return_defer(false);
        }

        if (fread(cRow, nChars * sizeof(char), 1, fp) != 1) {
            return_defer(false);
        }

        cRow[nChars + 1] = '\0';
        row_append(cRow, nChars, &linesRead);
        if (cRow != NULL)
            free(cRow);
    }

    return_defer(true);

defer:
    if (fp) {
        fclose(fp);
    }
    if (!result) {
        crash("file error");
    }
}

/*** append buffer ***/

struct abuf ab_init() {
    // initial size 8, randomly selected can change.
    int initial_capacity = 8;
    struct abuf ab = {0};

    // malloc() causes an error in realloc for ab_append? unsure why. calloc
    // seems to fix this issue.
    
    /*ab.buf = (char*)malloc(sizeof(char) * initial_capacity);*/
    ab.buf = (char*)calloc(sizeof(char), sizeof(char) * initial_capacity);
    ab.capacity = 8;
    ab.len = 0;


    return ab;
}

void ab_append(struct abuf* ab, const char* s, int len) {
    // Realloc memory, keep increasing until enough memory allocated.
    if (ab->len + len >= ab->capacity) {
        int new_capacity = ab->capacity;
        while (ab->len + len > new_capacity) {
            new_capacity *= 2;
        }
        ab->buf = (char*)realloc(ab->buf, new_capacity * sizeof(char));
        if (ab->buf == NULL) {
            crash("Realloc");
        }
        ab->capacity = new_capacity;
    }

    // copy new string into buffer.
    memcpy(&ab->buf[ab->len], s, len);
    ab->len += len;
}

void ab_free(struct abuf* ab) {
    if (ab->buf != NULL) {
        free(ab->buf);
    }
}

/*** output ***/

void tvim_scroll() {
    tvimConfig.rX = 0;
    if (tvimConfig.cY < tvimConfig.nRows) {
        tvimConfig.rX =
            row_cX_to_rX(&tvimConfig.rows[tvimConfig.cY], tvimConfig.cX);
    }

    if (tvimConfig.cY < tvimConfig.rowOff) {
        tvimConfig.rowOff = tvimConfig.cY;
    }
    if (tvimConfig.cY >= tvimConfig.rowOff + tvimConfig.screenRows) {
        tvimConfig.rowOff = tvimConfig.cY - tvimConfig.screenRows + 1;
    }
    if (tvimConfig.rX < tvimConfig.colOff) {
        tvimConfig.colOff = tvimConfig.rX;
    }
    if (tvimConfig.rX >= tvimConfig.colOff + tvimConfig.screenCols) {
        tvimConfig.colOff = tvimConfig.rX - tvimConfig.screenCols + 1;
    }
}

void tvim_move_cursor(int key) {
    row_t* row = (tvimConfig.cY >= tvimConfig.nRows)
                     ? NULL
                     : &tvimConfig.rows[tvimConfig.cY];

    switch (key) {
    case ARROW_LEFT:
        if (tvimConfig.cX != 0) {
            tvimConfig.cX--;
        } else if (tvimConfig.cY > 0) {
            tvimConfig.cY--;
            tvimConfig.cX = tvimConfig.rows[tvimConfig.cY].len;
        }
        break;
    case ARROW_RIGHT:
        if (row && tvimConfig.cX < row->len) {
            tvimConfig.cX++;
        } else if (row && tvimConfig.cX == row->len) {
            tvimConfig.cY++;
            tvimConfig.cX = 0;
        }
        break;
    case ARROW_UP:
        if (tvimConfig.cY != 0) {
            tvimConfig.cY--;
        }
        break;
    case ARROW_DOWN:
        if (tvimConfig.cY < tvimConfig.nRows) {
            tvimConfig.cY++;
        }
        break;
    }

    row = (tvimConfig.cY >= tvimConfig.nRows) ? NULL
                                              : &tvimConfig.rows[tvimConfig.cY];
    int rowlen = row ? row->len : 0;
    if (tvimConfig.cX > rowlen) {
        tvimConfig.cX = rowlen;
    }
}

void editorDrawRows(struct abuf* ab) {
    int y;
    for (y = 0; y < tvimConfig.screenRows; y++) {
        int filrow_t = y + tvimConfig.rowOff;
        if (filrow_t >= tvimConfig.nRows) {
            if (tvimConfig.nRows == 0 && y == tvimConfig.screenRows / 3) {
                char welcome[80];
                int welcomelen =
                    snprintf(welcome, sizeof(welcome),
                             "Kilo editor -- version %s", TVIM_VERSION);
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
        } else {
            int len = tvimConfig.rows[filrow_t].rlen - tvimConfig.colOff;
            if (len < 0)
                len = 0;
            if (len > tvimConfig.screenCols)
                len = tvimConfig.screenCols;
            ab_append(ab, &tvimConfig.rows[filrow_t].render[tvimConfig.colOff],
                      len);
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf* ab) {
    ab_append(ab, "\x1b[7m", 4);
    int len = 0;
    while (len < tvimConfig.screenCols) {
        ab_append(ab, " ", 1);
        len++;
    }
    ab_append(ab, "\x1b[m", 3);
}

void tvim_refresh_screen() {
    tvim_scroll();

    struct abuf ab = ab_init();

    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (tvimConfig.cY - tvimConfig.rowOff) + 1,
             (tvimConfig.rX - tvimConfig.colOff) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    ab_free(&ab);
}

void tvim_write_char(char c) {
    // TODO
    UNUSED(c);
}

void tvim_delete_char() {
    // TODO
}

/*** input ***/

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

void tvim_process_normal(int c) {

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
}

void tvim_process_visual(int c) {
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
}

void tvim_process_insert(int c) {
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
}

void tvim_process_key(int c) {
    switch (tvimConfig.tvimMode) {
    case INSERT:
        tvim_process_insert(c);
        break;
    case NORMAL:
        tvim_process_normal(c);
        break;
    case VISUAL:
        tvim_process_visual(c);
        break;
    default:
        break;
    }
}

/*** init ***/

void tvim_init() {
    tvimConfig.cX = 0;
    tvimConfig.cY = 0;
    tvimConfig.rX = 0;
    tvimConfig.rowOff = 0;
    tvimConfig.colOff = 0;
    tvimConfig.nRows = 0;
    tvimConfig.rows = NULL;
    tvimConfig.filename = NULL;

    if (terminal_get_window_size(&tvimConfig.screenRows,
                                 &tvimConfig.screenCols) == -1)
        crash("terminal_get_window_size");
    tvimConfig.screenRows -= 1;
}

/*** command line ***/

int command_valid(int argc, char** argv) {
    (void)argv;
    if (argc < 2) {
        return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    if (!command_valid(argc, argv)) {
        usage_error();
    }
    terminal_enable_raw_mode();
    tvim_init();
    file_open(argv[1]);
    while (1) {
        tvim_refresh_screen();
        int c = tvim_read_key();
        tvim_process_key(c);
    }
    return 0;
}
