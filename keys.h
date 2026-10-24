#ifndef FASTLESS_KEYS_H
#define FASTLESS_KEYS_H
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

/* Pager commands, named as in lesskey(1) where less has them. */
typedef enum {
    A_INVALID, A_NOACTION, A_DIGIT,
    A_FORW_LINE, A_BACK_LINE, A_FORW_LINE_FORCE, A_BACK_LINE_FORCE,
    A_FORW_SCREEN, A_BACK_SCREEN, A_FORW_SCREEN_FORCE, A_FORW_WINDOW, A_BACK_WINDOW,
    A_FORW_SCROLL, A_BACK_SCROLL, A_RIGHT_SCROLL, A_LEFT_SCROLL, A_RIGHT_END, A_LEFT_END,
    A_FORW_FOREVER, A_FORW_UNTIL_HILITE, A_REPAINT, A_FLUSH_REPAINT,
    A_F_SEARCH, A_B_SEARCH, A_AGAIN_SEARCH, A_REVERSE_SEARCH,
    A_T_AGAIN_SEARCH, A_T_REVERSE_SEARCH, A_UNDO_HILITE, A_CLEAR_SEARCH, A_FILTER,
    A_GOLINE, A_GOEND, A_PERCENT, A_GOPOS, A_F_BRACKET, A_B_BRACKET,
    A_SETMARK, A_SETMARKBOT, A_GOMARK, A_CLRMARK,
    A_EXAMINE, A_NEXT_FILE, A_PREV_FILE, A_INDEX_FILE, A_REMOVE_FILE, A_STAT,
    A_OPT_TOGGLE, A_DISP_OPTION, A_FIRSTCMD, A_SHELL, A_PSHELL, A_PIPE, A_SAVE,
    A_VISUAL, A_VERSION, A_HELP, A_QUIT, A_X11MOUSE, A_X116MOUSE,
    A_REGEX_MODE, A_LITERAL_MODE, A_GLOB_MODE, A_PREFIX
} Action;
/* Line-editing commands for prompts. E_INSERT means an ordinary character. */
typedef enum {
    E_INSERT, E_BACKSPACE, E_DELETE, E_WORD_BACKSPACE, E_WORD_DELETE, E_LEFT, E_RIGHT,
    E_WORD_LEFT, E_WORD_RIGHT, E_HOME, E_END, E_UP, E_DOWN, E_LITERAL, E_KILL_LINE,
    E_ABORT, E_IGNORE, E_PREFIX
} EditAction;

enum { KEY_EOF = -1, KEY_SIGNAL = -2, KEY_TIMEOUT = -3, KEY_WAKE = -4 };
extern int tty_fd;
/* Watched by key_wait as well as the terminal, when not -1. */
extern int wake_fd;
extern volatile sig_atomic_t resized, exiting, suspended;

void signals_install(void);
/* One input byte, from queued input first. KEY_SIGNAL when a signal flag is
   pending; KEY_TIMEOUT after timeout_ms (negative waits forever). */
int key_read(int timeout_ms);
/* As key_read, but KEY_WAKE when wake_fd becomes readable first; the caller
   drains it. */
int key_wait(int timeout_ms);
/* Queues text to be read before the terminal, after anything queued so far,
   or (front) before it. */
void keys_queue(const char *text, size_t len, bool front);
void keys_discard(void);
bool keys_queued(void);
/* Whether terminal input is ready to read now. */
bool keys_waiting(void);
/* Reads one command sequence. `extra` receives a lesskey extra string. */
Action read_command(char *extra, size_t cap);
/* Classifies an input sequence for line editing, reading more bytes of an
   escape sequence as needed; *c is the first byte. */
EditAction read_edit(int *c);

/* Loads lesskey source text. Defaults are loaded first by keys_defaults(). */
bool lesskey_parse(const char *text, const char *where, char *error, size_t cap);
bool lesskey_file(const char *path, bool required, char *error, size_t cap);
void keys_defaults(void);
#endif
