#ifndef TVIM_H
#define TVIM_H

#include <stddef.h>
#include <termios.h>

#define TVIM_VERSION "0.0.1"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

enum mode {
    NORMAL,
    VISUAL,
    INSERT,
};

enum key {
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
    size_t length;
    size_t capacity;
};

typedef struct {
    char* row;
    size_t len;
} row_t;

struct editorConfig {
    int curX, curY;
    size_t screenRows;
    size_t screenCols;
    struct termios original_termios;
    enum mode tvimMode;
    
    size_t num_rows;
    row_t* rows;
};

#endif
