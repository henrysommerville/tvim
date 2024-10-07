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

/*** data ***/

struct editorConfig tvimConfig;

/*** Exit ***/

void free_tvim() {
    if (tvimConfig.filename != NULL) {
        free(tvimConfig.filename);
        tvimConfig.filename = NULL;
    }
    if (tvimConfig.rows != NULL) {

        for (int i = 0; i < tvimConfig.nRows; i++) {
            if (tvimConfig.rows[i].chars != NULL) {
                free(tvimConfig.rows[i].chars);
                tvimConfig.rows[i].chars = NULL;
            }
            if (tvimConfig.rows[i].render != NULL) {
                free(tvimConfig.rows[i].render);
                tvimConfig.rows[i].render = NULL;
            }
        }
        free(tvimConfig.rows);
        tvimConfig.rows = NULL;
    }
    return;
}

void crash(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    free_tvim();
    perror(s);
    exit(99);
}

void clean_exit() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    free_tvim();
    exit(0);
}

void usage_error() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    fprintf(stderr, "Usage: tvim file");
    exit(1);
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

void row_update(row_t* row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->len; j++)
        if (row->chars[j] == '\t')
            tabs++;

    if (row->render != NULL) {
        free(row->render);
    }
    row->render = NULL;
    row->render = (char*)malloc(sizeof(char) *
                                (row->len + tabs * (TVIM_TAB_STOP - 1) + 1));
    if (row->render == NULL) {
        crash("malloc");
    }

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

void row_append(char* s, size_t len) {
    if (s == NULL) {
        return;
    }
    if (tvimConfig.nRows == 0) {
        tvimConfig.rows =
            (row_t*)malloc(sizeof(row_t) * (tvimConfig.nRows + 1));
    } else {
        tvimConfig.rows = (row_t*)realloc(
            tvimConfig.rows, sizeof(row_t) * (tvimConfig.nRows + 1));
    }

    if (tvimConfig.rows == NULL) {
        crash("malloc/realloc");
    }

    int at = tvimConfig.nRows;
    tvimConfig.rows[at].len = len;
    tvimConfig.rows[at].chars = (char*)malloc(len + 1);
    memcpy(tvimConfig.rows[at].chars, s, len);
    tvimConfig.rows[at].chars[len] = '\0';

    tvimConfig.rows[at].rlen = 0;
    tvimConfig.rows[at].render = NULL;
    row_update(&tvimConfig.rows[at]);
    tvimConfig.nRows++;
}

void row_insert(int at, char* s, size_t len) {
    if (at < 0 || at > tvimConfig.nRows)
        return;

    tvimConfig.rows = (row_t*)realloc(tvimConfig.rows,
                                      sizeof(row_t) * (tvimConfig.nRows + 1));
    if (tvimConfig.rows == NULL) {
        crash("realloc");
    }
    memmove(&tvimConfig.rows[at + 1], &tvimConfig.rows[at],
            sizeof(row_t) * (tvimConfig.nRows - at));

    tvimConfig.rows[at].len = len;
    tvimConfig.rows[at].chars = (char*)malloc(len + 1);
    if (tvimConfig.rows[at].chars == NULL) {
        crash("malloc");
    }

    memcpy(tvimConfig.rows[at].chars, s, len);
    tvimConfig.rows[at].chars[len] = '\0';

    tvimConfig.rows[at].rlen = 0;

    tvimConfig.rows[at].render = NULL;

    row_update(&tvimConfig.rows[at]);

    tvimConfig.nRows++;
    tvimConfig.unsaved++;
}

void row_free(row_t* row) {
    if (row->chars != NULL) {
        free(row->chars);
        row->chars = NULL;
    }

    if (row->render != NULL) {
        free(row->render);
        row->render = NULL;
    }

    // cannot free row since it is part of an array. Duh (I think)
    /*if (row != NULL) {*/
    /*    free(row);*/
    /*    row = NULL;*/
    /*}*/

    return;
}

void row_delete(int at) {
    if (at < 0 || at >= tvimConfig.nRows) {
        return;
    }

    row_free(&tvimConfig.rows[at]);

    size_t size = sizeof(row_t) * (tvimConfig.nRows - at - 1);
    memmove(&tvimConfig.rows[at], &tvimConfig.rows[at + 1], size);
    tvimConfig.nRows--;
    tvimConfig.unsaved++;

    return;
}

void row_join(row_t* row, char* s, int len) {
    row->chars = realloc(row->chars, row->len + len + 1);
    memcpy(&row->chars[row->len], s, len);
    row->len += len;
    row->chars[row->len] = '\0';
    row_update(row);
    tvimConfig.unsaved++;

    return;
}

/*** file i/o ***/

void file_get_lines(FILE* fp) {
    bool result = true;

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

            cRow[nChars] = '\0';
            row_append(cRow, nChars);
            free(cRow);
            cRow = NULL;
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
        row_append(cRow, nChars);
        free(cRow);
        cRow = NULL;
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

void file_open(char* filename) {
    tvimConfig.filename = strdup(filename);

    FILE* fp = fopen(filename, "r");
    if (!fp)
        crash("fopen");
    file_get_lines(fp);
}

void file_save() {

    FILE* fp = fopen(tvimConfig.filename, "r+");
    for (int i = 0; i < tvimConfig.nRows; i++) {
        // + 2 to include /n and /0
        char* row = (char*)malloc(sizeof(char) * tvimConfig.rows[i].len + 2);
        memcpy(row, tvimConfig.rows[i].chars, tvimConfig.rows[i].len);
        row[tvimConfig.rows[i].len] = '\n';

        fwrite(row, tvimConfig.rows[i].len + 1, 1, fp);

        free(row);
    }

    if (fp) {
        fclose(fp);
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
    case h:
    case ARROW_LEFT:
        if (tvimConfig.cX != 0) {
            tvimConfig.cX--;
        } else if (tvimConfig.cY > 0) {
            tvimConfig.cY--;
            tvimConfig.cX = tvimConfig.rows[tvimConfig.cY].len;
        }
        break;
    case l:
    case ARROW_RIGHT:
        if (row && tvimConfig.cX < row->len) {
            tvimConfig.cX++;
        } else if (row && tvimConfig.cX == row->len) {
            if (tvimConfig.cY < tvimConfig.nRows) {
                tvimConfig.cY++;
            }
            tvimConfig.cX = 0;
        }
        break;
    case k:
    case ARROW_UP:
        if (tvimConfig.cY != 0) {
            tvimConfig.cY--;
        }
        break;
    case j:
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

void tvim_draw_rows(struct abuf* ab) {
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

void tvim_draw_status(struct abuf* ab) {
    ab_append(ab, "\x1b[7m", 4);
    int len = 0;
    switch (tvimConfig.tvimMode) {
    case (NORMAL):
        ab_append(ab, "Normal", 6);
        len += 6;
        break;
    case (VISUAL):
        ab_append(ab, "Visual", 6);
        len += 6;
        break;
    case (INSERT):
        ab_append(ab, "Insert", 6);
        len += 6;
        break;
    case (COMMAND):
        ab_append(ab, ": ", 2);
        len += 2;
        break;
    default:
        break;
    }
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

    tvim_draw_rows(&ab);
    tvim_draw_status(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (tvimConfig.cY - tvimConfig.rowOff) + 1,
             (tvimConfig.rX - tvimConfig.colOff) + 1);
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.buf, ab.len);
    ab_free(&ab);
}

void tvim_new_line() {
    int line = tvimConfig.cY;
    if (tvimConfig.cX == 0) {
        row_insert(tvimConfig.cY, "", 0);
    } else {
        row_t* curRow = &tvimConfig.rows[line];
        row_insert(tvimConfig.cY + 1, &curRow->chars[tvimConfig.cX],
                   curRow->len - tvimConfig.cX);
        row_t* row = &tvimConfig.rows[tvimConfig.cY];
        row->len = tvimConfig.cX;
        row->chars[row->len] = '\0';
        row->render = NULL;
        row->rlen = 0;
        row_update(row);
    }
    tvimConfig.cY++;
    tvimConfig.cX = 0;

    return;
}

void tvim_write_char(char c) {
    if (tvimConfig.cY == tvimConfig.nRows) {
        row_append("", 0);
    }

    row_t* curRow = &tvimConfig.rows[tvimConfig.cY];

    int loc = tvimConfig.cX;
    if (loc < 0 || loc > curRow->len) {
        loc = curRow->len;
    }

    if (curRow->chars == NULL) {
        crash("Chars is null for some reason");
    }
    curRow->chars = (char*)realloc(curRow->chars, curRow->len + 2);
    if (curRow->chars == NULL) {
        crash("realloc");
    }
    if (memmove(&curRow->chars[loc + 1], &curRow->chars[loc],
                curRow->len - tvimConfig.cX + 1) == NULL) {
        crash("memmove");
    }

    curRow->len += 1;
    curRow->chars[loc] = c;
    tvimConfig.cX++;
    curRow->rlen = 0;
    curRow->render = NULL;
    row_update(curRow);

    return;
}

void tvim_delete_char() {
    row_t* curRow = &tvimConfig.rows[tvimConfig.cY];
    int loc = tvimConfig.cX - 1;

    if (tvimConfig.cY == tvimConfig.nRows) {
        return;
    }
    if (tvimConfig.cY == 0 && tvimConfig.cX == 0) {
        return;
    }

    if (tvimConfig.cX <= 0) {
        tvimConfig.cX = tvimConfig.rows[tvimConfig.cY - 1].len;

        row_join(&tvimConfig.rows[tvimConfig.cY - 1], curRow->chars,
                 curRow->len);

        row_delete(tvimConfig.cY);

        tvimConfig.cY--;
        tvimConfig.unsaved += 1;
    } else {

        if (loc > curRow->len) {
            loc = curRow->len;
        }

        memmove(&curRow->chars[loc], &curRow->chars[loc + 1],
                curRow->len - loc);

        curRow->len--;
        curRow->chars[curRow->len] = '\0';
        tvimConfig.cX--;

        row_update(curRow);
        tvimConfig.unsaved += 1;
    }

    return;
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
    case l:
    case k:
    case j:
    case h:
        tvim_move_cursor(c);
        break;
    case o:
        row_insert(tvimConfig.cY + 1, "", 0);
        tvimConfig.cY += 1;
        tvimConfig.cX = 0;
        tvimConfig.tvimMode = INSERT;
        break;

    case 'O':
        row_insert(tvimConfig.cY, "", 0);
        tvimConfig.cX = 0;
        tvimConfig.tvimMode = INSERT;
        break;
    case DELETE_KEY:
        tvim_move_cursor(ARROW_RIGHT);
        tvim_delete_char();
        tvimConfig.tvimMode = NORMAL;
        break;
    case v:
        tvimConfig.tvimMode = VISUAL;
        break;
    case i:
        tvimConfig.tvimMode = INSERT;
        break;
    case ESCAPE:
        break;
    case CTRL_KEY('q'):
        clean_exit();
        break;
    case CTRL_KEY('s'):
        file_save();
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
        tvim_move_cursor(ARROW_RIGHT);
        tvim_delete_char();
        tvimConfig.tvimMode = NORMAL;
        break;
    case ESCAPE:
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
        tvim_move_cursor(ARROW_RIGHT);
        tvim_delete_char();
        tvimConfig.tvimMode = NORMAL;
        break;
    case BACKSPACE:
        tvim_delete_char();
        break;
    case ENTER:
        tvim_new_line();
        break;
    case CTRL_KEY('l'):
    case ESCAPE:
        tvimConfig.tvimMode = NORMAL;
        break;
    case CTRL_KEY('q'):
        clean_exit();
        break;
    default:
        tvim_write_char(c);
        break;
    }

    return;
}

void tvim_process_command(int c) {
    UNUSED(c);
    return;
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
    case COMMAND:
        tvim_process_command(c);
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
    int i = 0;
    while (1) {
        tvim_refresh_screen();
        int c = tvim_read_key();
        i++;
        tvim_process_key(c);
    }
    free_tvim();
    return 0;
}
