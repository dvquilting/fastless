#ifndef FASTLESS_OPTIONS_H
#define FASTLESS_OPTIONS_H
#include <stdbool.h>
#include <stddef.h>
#include "input.h"

enum { PROMPT_SHORT, PROMPT_MEDIUM, PROMPT_LONG, PROMPT_EQUALS, PROMPT_HELP, PROMPT_WAIT, PROMPT_COUNT };
typedef struct {
    /* Searching. mode is a Mode; case_mode 0 sensitive, 1 -i, 2 -I. */
    int mode, case_mode;
    int skip_screen;          /* 1: -a, 2: -A */
    int hilite;               /* 0 all matches, 1 -g last match, 2 -G none */
    char search_options[8];   /* --search-options letters: E F K N R W */
    int incsearch, no_search_header_lines, no_search_header_cols;
    /* Moving and display. */
    int quit_eof;             /* 1: -e, 2: -E */
    int quit_one_screen, quit_intr, no_init, clear, squeeze, tilde, status_column, status_line;
    int quiet;                /* 1: -q, 2: -Q */
    int window;               /* -z: 0 default, negative counts from the screen height */
    int jump_line;            /* -j: 1-based row, negative from the bottom */
    double jump_fraction;     /* -j.N, used when jump_is_fraction */
    int jump_is_fraction;
    int shift;                /* -#: 0 is half the screen width */
    double shift_fraction;
    int shift_is_fraction;
    int unread;               /* 1: -w, 2: -W */
    int prompt_style;         /* 0 fastless status, 1 -m, 2 -M */
    char *prompt[PROMPT_COUNT];
    int header_lines, header_cols;
    size_t header_start;      /* 1-based line; 0 means the first line */
    int status_width, rscroll, match_shift, mouse, wheel_lines, redraw_on_quit;
    int save_marks, no_histdups, follow_name, exit_follow_on_close, intr_char;
    int line_numbers;         /* 1: -n, none even in prompts; 2: -N, a column */
    int line_num_width, no_number_headers;
    int raw;                  /* -r/-R: pass color sequences */
    int special;              /* 1: -u, 2: -U */
    int proc_backspace, proc_return, proc_tab; /* 1 --proc-x, 2 --PROC-X */
    /* Input. */
    int force, no_lessopen, show_preproc_errors;
    size_t max_memory;
    const char *temp_dir, *simd;
    unsigned threads;
    Compression compression;
    /* Noninteractive search. */
    const char *find;
    int all;
    size_t offset;
    const char *lesskey_src, *lesskey_content;
} Options;
extern Options opt;

typedef struct OptionDef OptionDef;
enum { HOW_SET, HOW_TOGGLE, HOW_RESET, HOW_OPPOSITE };

void options_init(void);
/* Parses an option string such as $LESS. Unknown options are ignored. */
void options_parse_env(const char *text);
/* Parses argv, collecting file arguments in `files`. Returns 0, or -1 with
   an error, or -3 for --help, or -4 for --version. */
int options_parse_argv(int argc, char **argv, char **files, int *file_count, char *error, size_t cap);
/* Startup commands from + options and -p, executed when a file opens. */
extern char *first_command, *every_command;

const OptionDef *option_short(int letter);
const OptionDef *option_long(const char *name, size_t len, char *error, size_t cap);
bool option_takes_value(const OptionDef *o);
bool option_runtime(const OptionDef *o);
bool option_supported(const OptionDef *o);
/* Applies one option; msg receives a description of the new state. */
bool option_apply(const OptionDef *o, const char *value, int how, char *msg, size_t cap);
void option_describe(const OptionDef *o, char *msg, size_t cap);
const char *option_name(const OptionDef *o);
#endif
