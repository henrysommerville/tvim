#ifndef TVIM_H
#define TVIM_H

#include <stddef.h>
#include <termios.h>

#define TVIM_VERSION "0.0.1"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define UNUSED(x) (void)x

#define TVIM_TAB_STOP 4

enum mode {
    NORMAL = 0,
    VISUAL,
    INSERT,
    COMMAND,
};

enum key {
    BACKSPACE = 127,
    i = 'i',
    ESCAPE = 27, 
    ENTER = '\r',
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DELETE_KEY,
    // could add page up/down. I don't want to, so I will not :)
    // could add home + end keys. but guess what? I don't use those so I will not.
};

struct abuf {
    char* buf;
    int len;
    int capacity;
};

typedef struct {
    int len;
    int rlen;
    char* chars;
    char* render;
} row_t;

struct editorConfig {
    int cX, cY;
    int rX;
    int rowOff;
    int colOff;
    int screenRows;
    int screenCols;
    int nRows;
    row_t* rows;
    char* filename;

    int unsaved;

    enum mode tvimMode;

    struct termios og_termios;
};

#endif
