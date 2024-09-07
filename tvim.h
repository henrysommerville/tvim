#ifndef TVIM_H
#define TVIM_H

#include <stddef.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>

/*** Defines ***/
#define TVIM_VERSION "0.0.1"
#define TVIM_TAB_STOP 4

/*** Macros ***/
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define UNUSED(x) (void)x

#define CTRL_KEY(k) ((k) & 0x1F)

#define return_defer(value) \
    do {                    \
        result = (value);   \
        goto defer;         \
    } while (0)

/*** Data types and Data def ***/

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


/******* FUNCTIONS *******/

/*** Exit ***/

void crash(const char* s);
void clean_exit();
void usage_error(); 

/*** terminal ***/

void terminal_disable_raw_mode();
void terminal_enable_raw_mode();
int terminal_get_cursor_position(int* rows, int* cols);
int terminal_get_window_size(int* rows, int* cols); 

/*** row operations ***/

int row_cX_to_rX(row_t* row, int cX); 
void row_update(row_t* row); 
void row_append(char* s, size_t len); 
void row_insert(int at, char* s, size_t len); 
void row_free(row_t* row); 
void row_delete(int at); 
void row_join(row_t* row, char* s, int len); 

/*** char operations ***/
void tvim_write_char(char c); 
void tvim_delete_char(); 
void tvim_new_line(); 

/*** file i/o ***/

void file_open(char* filename); 
void file_get_rows(FILE* fp);

/*** append buffer ***/

struct abuf ab_init(); 
void ab_append(struct abuf* ab, const char* s, int len); 
void ab_free(struct abuf* ab); 

/*** output ***/

void tvim_scroll(); 

void tvim_move_cursor(int key); 

void tvim_draw_rows(struct abuf* ab); 

void tvim_draw_status(struct abuf* ab); 

void tvim_refresh_screen(); 

/*** input ***/

int tvim_read_key(); 

void tvim_process_normal(int c); 

void tvim_process_visual(int c); 

void tvim_process_insert(int c); 

void tvim_process_command(int c); 

void tvim_process_key(int c); 

/*** init ***/

void tvim_init(); 

/*** command line ***/

int command_valid(int argc, char** argv); 

int main(int argc, char** argv); 


#endif
