#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "options.h"
#include "render.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Options opt;
char *first_command, *every_command;

typedef enum { O_FLAG, O_INT, O_FUNC, O_NOOP, O_UNSUPPORTED } Kind;
enum {
    ARG = 1,        /* takes a value */
    STRING = 2,     /* the value runs to '$' in $LESS, not just to a non-digit */
    START = 4,      /* only meaningful at startup */
    OPTIONAL = 8,   /* the value may be absent */
};
struct OptionDef {
    char letter;
    const char *name;
    Kind kind;
    int *var;
    int value;      /* O_FLAG: value set; O_INT: default */
    bool (*func)(const char *value, char *error, size_t cap);
    unsigned flags;
    const char *description;
};

static void set_string(char **slot, const char *value) {
    char *copy = strdup(value);
    if (copy) { free(*slot); *slot = copy; }
}
/* Stores a copy of a string option's value: values from $LESS and from
   option clusters live in buffers that are freed once applied. A replaced
   value is not freed, as it may be a default literal. */
static bool keep(const char **slot, const char *value, char *error, size_t cap) {
    char *copy = strdup(value);
    if (!copy) { snprintf(error, cap, "Out of memory"); return false; }
    *slot = copy;
    return true;
}
/* Signed decimal with an optional fraction, as for -j and -#. */
static bool parse_number(const char *v, long *whole, double *fraction, bool *is_fraction) {
    char *end;
    if (*v == '.') {
        errno = 0;
        double f = strtod(v, &end);
        if (*end || errno || f < 0 || f >= 1) return false;
        *fraction = f; *is_fraction = true;
        return true;
    }
    errno = 0;
    long n = strtol(v, &end, 10);
    if (end == v || *end || errno || n < -100000000 || n > 100000000) return false;
    *whole = n; *is_fraction = false;
    return true;
}
static bool f_jump(const char *v, char *error, size_t cap) {
    long n = 1; double f = 0; bool frac;
    if (!parse_number(v, &n, &f, &frac)) { snprintf(error, cap, "Invalid -j value"); return false; }
    opt.jump_is_fraction = frac;
    if (frac) opt.jump_fraction = f; else opt.jump_line = n ? (int)n : 1;
    return true;
}
static bool f_shift(const char *v, char *error, size_t cap) {
    long n = 0; double f = 0; bool frac;
    if (!parse_number(v, &n, &f, &frac) || n < 0) { snprintf(error, cap, "Invalid -# value"); return false; }
    opt.shift_is_fraction = frac;
    if (frac) opt.shift_fraction = f; else opt.shift = (int)n;
    return true;
}
static bool f_window(const char *v, char *error, size_t cap) {
    long n; double f; bool frac;
    if (!parse_number(v, &n, &f, &frac) || frac) { snprintf(error, cap, "Invalid -z value"); return false; }
    opt.window = (int)n;
    return true;
}
static bool f_tabs(const char *v, char *error, size_t cap) {
    int tabs[32], count = 0;
    for (const char *p = v; *p && count < 32;) {
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p || n < 1 || n > 1000 || (count && n <= tabs[count - 1])) {
            snprintf(error, cap, "Invalid tab stops");
            return false;
        }
        tabs[count++] = (int)n;
        p = *end == ',' ? end + 1 : end;
        if (*end && *end != ',') { snprintf(error, cap, "Invalid tab stops"); return false; }
    }
    if (!count) { snprintf(error, cap, "Invalid tab stops"); return false; }
    render_tabs(tabs, count);
    return true;
}
static bool f_prompt(const char *v, char *error, size_t cap) {
    (void)error; (void)cap;
    int which = PROMPT_SHORT;
    const char *kinds = "smM=hw";
    const char *k = *v ? strchr(kinds, *v) : NULL;
    if (k) { which = (int)(k - kinds); ++v; }
    set_string(&opt.prompt[which], v);
    return true;
}
static bool f_header(const char *v, char *error, size_t cap) {
    long values[3] = {-1, -1, -1};
    const char *p = v;
    for (int i = 0; i < 3 && *p; ++i) {
        if (*p != ',') {
            char *end;
            values[i] = strtol(p, &end, 10);
            if (end == p || values[i] < 0 || values[i] > 1000000000) {
                snprintf(error, cap, "Invalid --header value");
                return false;
            }
            p = end;
        }
        if (*p == ',') ++p;
        else if (*p) { snprintf(error, cap, "Invalid --header value"); return false; }
    }
    if (values[0] >= 0) opt.header_lines = (int)values[0];
    if (values[1] >= 0) opt.header_cols = (int)values[1];
    opt.header_start = values[2] > 0 ? (size_t)values[2] : 0;
    return true;
}
static int parse_char(const char *v) {
    if (v[0] == '^' && v[1] && !v[2]) return v[1] == '?' ? 0x7f : v[1] & 0x1f;
    return v[0] && !v[1] ? (unsigned char)v[0] : -1;
}
static bool f_rscroll(const char *v, char *error, size_t cap) {
    if (!strcmp(v, "-")) { opt.rscroll = 0; return true; }
    int c = parse_char(v);
    if (c < 0x20 || c > 0x7e) { snprintf(error, cap, "Invalid --rscroll character"); return false; }
    opt.rscroll = c;
    return true;
}
static bool f_intr(const char *v, char *error, size_t cap) {
    int c = parse_char(v);
    if (c < 0) { snprintf(error, cap, "Invalid --intr character"); return false; }
    opt.intr_char = c;
    return true;
}
static bool f_search_options(const char *v, char *error, size_t cap) {
    char out[8] = "";
    size_t n = 0;
    for (; *v; ++v) {
        if (*v == '-') { n = 0; continue; }
        if (!strchr("EFKNRW", *v)) { snprintf(error, cap, "Invalid --search-options letter"); return false; }
        if (!memchr(out, *v, n) && n < sizeof(out) - 1) out[n++] = *v;
    }
    out[n] = 0;
    memcpy(opt.search_options, out, sizeof(out));
    return true;
}
/* A non-negative decimal number of GiB, such as 10 or 0.5. */
static bool f_max_mem(const char *v, char *error, size_t cap) {
    size_t digits = strspn(v, "0123456789");
    size_t fraction = v[digits] == '.' ? strspn(v + digits + 1, "0123456789") : 0;
    double n = strtod(v, NULL) * 1073741824.0;
    if ((!digits && !fraction) || v[digits + (v[digits] == '.') + fraction] ||
        !(n < 9223372036854775808.0)) {
        snprintf(error, cap, "--max-mem must be a non-negative number of GiB");
        return false;
    }
    opt.max_memory = (size_t)n;
    return true;
}
static bool f_threads(const char *v, char *error, size_t cap) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(v, &end, 10);
    if (!*v || *v == '-' || *end || errno || n > 256) {
        snprintf(error, cap, "Threads must be between 0 and 256");
        return false;
    }
    opt.threads = (unsigned)n;
    return true;
}
static bool f_offset(const char *v, char *error, size_t cap) {
    char *end;
    errno = 0;
    unsigned long long n = strtoull(v, &end, 10);
    if (!*v || *v == '-' || *end || errno || n > SIZE_MAX) {
        snprintf(error, cap, "Invalid byte offset");
        return false;
    }
    opt.offset = (size_t)n;
    return true;
}
static bool f_compression(const char *v, char *error, size_t cap) {
    const char *names[] = {"auto", "none", "gzip", "zstd", "xz", "bzip2"};
    for (int i = 0; i < 6; ++i)
        if (!strcmp(v, names[i])) { opt.compression = (Compression)i; return true; }
    snprintf(error, cap, "Invalid compression format");
    return false;
}
static bool f_simd(const char *v, char *error, size_t cap) {
    if (strcmp(v, "auto") && strcmp(v, "off") && strcmp(v, "avx2")) {
        snprintf(error, cap, "Invalid SIMD mode; use auto, off, or avx2");
        return false;
    }
    return keep(&opt.simd, v, error, cap);
}
static bool f_tmpdir(const char *v, char *error, size_t cap) { return keep(&opt.temp_dir, v, error, cap); }
static bool f_find(const char *v, char *error, size_t cap) { return keep(&opt.find, v, error, cap); }
static bool f_lesskey_src(const char *v, char *error, size_t cap) { return keep(&opt.lesskey_src, v, error, cap); }
static bool f_lesskey_content(const char *v, char *error, size_t cap) { return keep(&opt.lesskey_content, v, error, cap); }
static bool f_no_search_headers(const char *v, char *error, size_t cap) {
    (void)v; (void)error; (void)cap;
    opt.no_search_header_lines = opt.no_search_header_cols = 1;
    return true;
}
static bool f_case_sensitive(const char *v, char *error, size_t cap) { (void)v; (void)error; (void)cap; opt.case_mode = 0; return true; }
static void add_command(char **slot, const char *prefix, const char *v) {
    size_t old = *slot ? strlen(*slot) : 0, n = strlen(prefix) + strlen(v);
    char *grown = realloc(*slot, old + n + 2);
    if (!grown) return;
    /* Commands that read a line end with RETURN, as typed. */
    snprintf(grown + old, n + 2, "%s%s%s", prefix, v, strchr("/?&:!#|-_s+", *prefix ? *prefix : *v) ? "\r" : "");
    *slot = grown;
}
static bool f_pattern(const char *v, char *error, size_t cap) {
    (void)error; (void)cap;
    add_command(&first_command, "/", v);
    return true;
}
static bool f_help(const char *v, char *error, size_t cap) { (void)v; (void)error; (void)cap; return true; }
static bool f_version(const char *v, char *error, size_t cap) { (void)v; (void)error; (void)cap; return true; }
static bool f_lesskey_file(const char *v, char *error, size_t cap) {
    (void)v;
    snprintf(error, cap, "Compiled lesskey files are not supported; use --lesskey-src");
    return false;
}

#define FLAG(l, n, var, val, f, d) {l, n, O_FLAG, &opt.var, val, NULL, f, d}
#define INT(l, n, var, def, f, d) {l, n, O_INT, &opt.var, def, NULL, ARG | (f), d}
#define FUNC(l, n, fn, f, d) {l, n, O_FUNC, NULL, 0, fn, f, d}
#define NOOP(l, n, f) {l, n, O_NOOP, NULL, 0, NULL, f, NULL}
#define UNSUPPORTED(l, n, f, d) {l, n, O_UNSUPPORTED, NULL, 0, NULL, f, d}
static const OptionDef options[] = {
    FLAG('a', "search-skip-screen", skip_screen, 1, 0, "Search skips the current screen"),
    FLAG('A', "SEARCH-SKIP-SCREEN", skip_screen, 2, 0, "Search starts just after the target line"),
    NOOP('b', "buffers", ARG),
    NOOP('B', "auto-buffers", 0),
    FLAG('c', "clear-screen", clear, 1, 0, "Repaint by clearing the screen"),
    FLAG('C', "CLEAR-SCREEN", clear, 1, 0, "Repaint by clearing the screen"),
    NOOP('d', "dumb", 0),
    UNSUPPORTED('D', "color", ARG | STRING, "colors"),
    FLAG('e', "quit-at-eof", quit_eof, 1, 0, "Quit the second time end of file is reached"),
    FLAG('E', "QUIT-AT-EOF", quit_eof, 2, 0, "Quit at end of file"),
    FLAG('f', "force", force, 1, START, "Open non-regular files"),
    FLAG('F', "quit-if-one-screen", quit_one_screen, 1, START, "Quit if the file fits on one screen"),
    FLAG('g', "hilite-search", hilite, 1, 0, "Highlight only the last match"),
    FLAG('G', "HILITE-SEARCH", hilite, 2, 0, "Do not highlight matches"),
    NOOP('h', "max-back-scroll", ARG),
    FLAG('i', "ignore-case", case_mode, 1, 0, "Ignore case in searches without uppercase"),
    FLAG('I', "IGNORE-CASE", case_mode, 2, 0, "Ignore case in all searches"),
    FUNC('j', "jump-target", f_jump, ARG, "Target line"),
    FLAG('J', "status-column", status_column, 1, 0, "Display a status column"),
    FUNC('k', "lesskey-file", f_lesskey_file, ARG | STRING | START, NULL),
    FLAG('K', "quit-on-intr", quit_intr, 1, 0, "Quit on interrupt"),
    FLAG('L', "no-lessopen", no_lessopen, 1, 0, "Ignore LESSOPEN"),
    FLAG('m', "long-prompt", prompt_style, 1, 0, "Medium prompt"),
    FLAG('M', "LONG-PROMPT", prompt_style, 2, 0, "Long prompt"),
    FLAG('n', "line-numbers", line_numbers, 1, 0, "Suppress line numbers"),
    FLAG('N', "LINE-NUMBERS", line_numbers, 2, 0, "Display line numbers"),
    UNSUPPORTED('o', "log-file", ARG | STRING, "log files"),
    UNSUPPORTED('O', "LOG-FILE", ARG | STRING, "log files"),
    FUNC('p', "pattern", f_pattern, ARG | STRING | START, NULL),
    FUNC('P', "prompt", f_prompt, ARG | STRING, "Prompt"),
    FLAG('q', "quiet", quiet, 1, 0, "Ring the bell only for errors"),
    FLAG('q', "silent", quiet, 1, 0, "Ring the bell only for errors"),
    FLAG('Q', "QUIET", quiet, 2, 0, "Never ring the bell"),
    FLAG('Q', "SILENT", quiet, 2, 0, "Never ring the bell"),
    FLAG('r', "raw-control-chars", raw, 1, 0, "Pass color sequences through"),
    FLAG('R', "RAW-CONTROL-CHARS", raw, 2, 0, "Pass color sequences through"),
    FLAG('s', "squeeze-blank-lines", squeeze, 1, 0, "Squeeze multiple blank lines"),
    NOOP('S', "chop-long-lines", 0),
    UNSUPPORTED('t', "tag", ARG | STRING, "tags"),
    UNSUPPORTED('T', "tag-file", ARG | STRING, "tags"),
    FLAG('u', "underline-special", special, 1, 0, "Show backspaces and carriage returns as control characters"),
    FLAG('U', "UNDERLINE-SPECIAL", special, 2, 0, "Show backspaces, tabs and carriage returns as control characters"),
    FUNC('V', "version", f_version, START, NULL),
    FLAG('w', "hilite-unread", unread, 1, 0, "Highlight the first new line after a page forward"),
    FLAG('W', "HILITE-UNREAD", unread, 2, 0, "Highlight the first new line after any forward movement"),
    FUNC('x', "tabs", f_tabs, ARG, "Tab stops"),
    FLAG('X', "no-init", no_init, 1, START, "Do not use the alternate screen"),
    NOOP('y', "max-forw-scroll", ARG),
    FUNC('z', "window", f_window, ARG, "Window size"),
    UNSUPPORTED('"', "quotes", ARG | STRING, "shell quote characters"),
    FLAG('~', "tilde", tilde, 1, 0, "Do not display tildes after end of file"),
    FUNC('#', "shift", f_shift, ARG, "Horizontal shift"),
    FUNC('?', "help", f_help, START, NULL),
    FLAG(0, "exit-follow-on-close", exit_follow_on_close, 1, 0, "Exit F on a closed pipe"),
    NOOP(0, "file-size", 0),
    FLAG(0, "follow-name", follow_name, 1, 0, "F follows the file name"),
    FUNC(0, "header", f_header, ARG | STRING, "Header lines and columns"),
    FLAG(0, "incsearch", incsearch, 1, 0, "Search while the pattern is typed"),
    FUNC(0, "intr", f_intr, ARG | STRING, "Interrupt character"),
    FUNC(0, "lesskey-content", f_lesskey_content, ARG | STRING | START, NULL),
    FUNC(0, "lesskey-src", f_lesskey_src, ARG | STRING | START, NULL),
    INT(0, "line-num-width", line_num_width, 7, 0, "Line number width"),
    INT(0, "match-shift", match_shift, 0, 0, "Columns kept left of a match"),
    UNSUPPORTED(0, "modelines", ARG, "modelines"),
    FLAG(0, "mouse", mouse, 1, 0, "Mouse input"),
    FLAG(0, "MOUSE", mouse, 2, 0, "Mouse input, wheel reversed"),
    NOOP(0, "no-keypad", 0),
    FLAG(0, "no-histdups", no_histdups, 1, 0, "Remove duplicates from history"),
    FLAG(0, "no-number-headers", no_number_headers, 1, 0, "Header lines are not numbered"),
    FLAG(0, "no-search-header-lines", no_search_header_lines, 1, 0, "Searches skip header lines"),
    FLAG(0, "no-search-header-columns", no_search_header_cols, 1, 0, "Searches skip header columns"),
    FUNC(0, "no-search-headers", f_no_search_headers, 0, "Searches skip header lines and columns"),
    NOOP(0, "no-vbell", 0),
    FLAG(0, "redraw-on-quit", redraw_on_quit, 1, 0, "Redraw the final screen when quitting"),
    FUNC(0, "rscroll", f_rscroll, ARG | STRING, "Truncation marker"),
    FLAG(0, "save-marks", save_marks, 1, 0, "Keep marks across invocations"),
    FUNC(0, "search-options", f_search_options, ARG | STRING | OPTIONAL, "Default search options"),
    FLAG(0, "show-preproc-errors", show_preproc_errors, 1, 0, "Report preprocessor errors"),
    FLAG(0, "proc-backspace", proc_backspace, 1, 0, "Process backspaces"),
    FLAG(0, "PROC-BACKSPACE", proc_backspace, 2, 0, "Show backspaces as control characters"),
    FLAG(0, "proc-return", proc_return, 1, 0, "Delete carriage returns before newlines"),
    FLAG(0, "PROC-RETURN", proc_return, 2, 0, "Show carriage returns as control characters"),
    FLAG(0, "proc-tab", proc_tab, 1, 0, "Expand tabs"),
    FLAG(0, "PROC-TAB", proc_tab, 2, 0, "Show tabs as control characters"),
    INT(0, "status-col-width", status_width, 2, 0, "Status column width"),
    FLAG(0, "status-line", status_line, 1, 0, "Highlight marked lines"),
    UNSUPPORTED(0, "use-backslash", 0, "--use-backslash"),
    UNSUPPORTED(0, "use-color", 0, "colors"),
    INT(0, "wheel-lines", wheel_lines, 1, 0, "Lines per mouse wheel step"),
    UNSUPPORTED(0, "wordwrap", 0, "line wrapping"),
    /* fastless's own options. */
    FLAG(0, "regex", mode, 0, 0, "Regular expression searches"),
    FLAG(0, "literal", mode, 1, 0, "Literal searches"),
    FLAG(0, "glob", mode, 2, 0, "Whole-line glob searches"),
    FUNC(0, "case-sensitive", f_case_sensitive, 0, NULL),
    FUNC(0, "find", f_find, ARG | STRING | START, NULL),
    FLAG(0, "all", all, 1, START, NULL),
    FUNC(0, "offset", f_offset, ARG | START, NULL),
    FUNC(0, "tmpdir", f_tmpdir, ARG | STRING | START, NULL),
    FUNC(0, "max-mem", f_max_mem, ARG | START, NULL),
    FUNC(0, "threads", f_threads, ARG | START, NULL),
    FUNC(0, "simd", f_simd, ARG | START, NULL),
    FUNC(0, "compression", f_compression, ARG | START, NULL),
};
#define OPTION_COUNT (sizeof(options) / sizeof(options[0]))

void options_init(void) {
    opt.max_memory = (size_t)10 << 30;
    opt.simd = "auto";
    opt.jump_line = 1;
    opt.status_width = 2;
    opt.rscroll = '>';
    opt.wheel_lines = 1;
    opt.line_num_width = 7;
    opt.intr_char = 0x18;
}
const OptionDef *option_short(int letter) {
    for (size_t i = 0; i < OPTION_COUNT; ++i)
        if (letter && options[i].letter == letter) return &options[i];
    return NULL;
}
/* Exact names first, then a unique prefix, as less allows. */
const OptionDef *option_long(const char *name, size_t len, char *error, size_t cap) {
    const OptionDef *found = NULL;
    int matches = 0;
    for (size_t i = 0; i < OPTION_COUNT; ++i)
        if (strlen(options[i].name) == len && !memcmp(options[i].name, name, len)) return &options[i];
    for (size_t i = 0; i < OPTION_COUNT; ++i)
        if (!strncmp(options[i].name, name, len) && strlen(options[i].name) > len) {
            if (!found || strcmp(found->name, options[i].name)) ++matches;
            found = &options[i];
        }
    if (matches == 1) return found;
    snprintf(error, cap, matches ? "Ambiguous option --%.*s" : "There is no --%.*s option", (int)len, name);
    return NULL;
}
bool option_takes_value(const OptionDef *o) { return o->flags & ARG; }
bool option_supported(const OptionDef *o) { return o->kind != O_UNSUPPORTED; }
bool option_runtime(const OptionDef *o) { return !(o->flags & START) && o->kind != O_NOOP && o->kind != O_UNSUPPORTED; }
const char *option_name(const OptionDef *o) { return o->name; }
void option_describe(const OptionDef *o, char *msg, size_t cap) {
    if (o->kind == O_FLAG) {
        bool on = *o->var == o->value && (o->value || o->var == &opt.mode);
        snprintf(msg, cap, "%s: %s", o->description ? o->description : o->name, on ? "on" : "off");
    } else if (o->kind == O_INT) snprintf(msg, cap, "%s: %d", o->description, *o->var);
    else if (o->func == f_jump) {
        if (opt.jump_is_fraction) snprintf(msg, cap, "Target line: %.3g of the screen", opt.jump_fraction);
        else snprintf(msg, cap, "Target line: %d", opt.jump_line);
    } else if (o->func == f_shift) {
        if (opt.shift_is_fraction) snprintf(msg, cap, "Horizontal shift: %.3g of the screen", opt.shift_fraction);
        else snprintf(msg, cap, "Horizontal shift: %d%s", opt.shift, opt.shift ? "" : " (half the screen)");
    } else if (o->func == f_window) snprintf(msg, cap, "Window size: %d%s", opt.window, opt.window ? "" : " (screen)");
    else if (o->func == f_tabs) {
        size_t n = (size_t)snprintf(msg, cap, "Tab stops:");
        for (int i = 0; i < render.tab_count && n < cap; ++i)
            n += (size_t)snprintf(msg + n, cap - n, "%s%d", i ? "," : " ", render.tabs[i]);
    }
    else if (o->func == f_header)
        snprintf(msg, cap, "Header: %d lines, %d columns", opt.header_lines, opt.header_cols);
    else if (o->func == f_rscroll) snprintf(msg, cap, "Truncation marker: %c", opt.rscroll ? opt.rscroll : '-');
    else if (o->func == f_search_options) snprintf(msg, cap, "Search options: %s", opt.search_options);
    else if (o->func == f_prompt)
        snprintf(msg, cap, "Prompt: %s", opt.prompt[PROMPT_SHORT] ? opt.prompt[PROMPT_SHORT] : "(default)");
    else if (o->func == f_no_search_headers)
        snprintf(msg, cap, "Searches skip header lines: %s, columns: %s",
                 opt.no_search_header_lines ? "on" : "off", opt.no_search_header_cols ? "on" : "off");
    else snprintf(msg, cap, "--%s", o->name);
}
bool option_apply(const OptionDef *o, const char *value, int how, char *msg, size_t cap) {
    char error[256] = "";
    switch (o->kind) {
    case O_UNSUPPORTED:
        snprintf(msg, cap, "fastless does not support %s (--%s)", o->description, o->name);
        return false;
    case O_NOOP:
        if (msg && cap) *msg = 0;
        return true;
    case O_FLAG:
        if (how == HOW_RESET) *o->var = 0;
        else if (how == HOW_TOGGLE || how == HOW_OPPOSITE)
            *o->var = *o->var == o->value && o->value ? 0 : o->value;
        else *o->var = o->value;
        break;
    case O_INT: {
        if (how == HOW_RESET || !value) { *o->var = o->value; break; }
        char *end;
        errno = 0;
        long n = strtol(value, &end, 10);
        if (!*value || *end || errno || n < 0 || n > 1000000) {
            snprintf(msg, cap, "Invalid value for --%s", o->name);
            return false;
        }
        *o->var = (int)n;
        break;
    }
    case O_FUNC:
        if (how == HOW_RESET) {
            if (o->func == f_jump) { opt.jump_line = 1; opt.jump_is_fraction = 0; }
            else if (o->func == f_shift) { opt.shift = 0; opt.shift_is_fraction = 0; }
            else if (o->func == f_window) opt.window = 0;
            else if (o->func == f_header) { opt.header_lines = opt.header_cols = 0; opt.header_start = 0; }
            else if (o->func == f_rscroll) opt.rscroll = '>';
            else if (o->func == f_no_search_headers) opt.no_search_header_lines = opt.no_search_header_cols = 0;
            else if (o->func == f_search_options) opt.search_options[0] = 0;
            else if (o->func == f_tabs) { int eight = 8; render_tabs(&eight, 1); }
            break;
        }
        if (!o->func(value ? value : "", error, sizeof(error))) {
            snprintf(msg, cap, "%s", error);
            return false;
        }
        break;
    }
    if (msg && cap) option_describe(o, msg, cap);
    return true;
}

/* The value of a short option inside a cluster: a string runs to the end
   (or to '$' in $LESS); a number is a signed decimal with a fraction. */
static size_t short_value(const OptionDef *o, const char *p, bool env) {
    if (o->flags & STRING) return env ? strcspn(p, "$") : strlen(p);
    size_t n = 0;
    if (p[n] == '-' || p[n] == '.') ++n;
    while (isdigit((unsigned char)p[n]) || p[n] == '.' || p[n] == ',') ++n;
    return n;
}
static bool apply_quietly(const OptionDef *o, const char *value, int how, char *error, size_t cap) {
    return option_apply(o, value, how, error, cap);
}
void options_parse_env(const char *text) {
    char error[256];
    for (const char *p = text; *p;) {
        if (isspace((unsigned char)*p)) { ++p; continue; }
        if (*p == '+') {
            size_t n = strcspn(p + 1, "$");
            char *cmd = strndup(p + 1, n);
            if (cmd) { add_command(&first_command, "", cmd); free(cmd); }
            p += 1 + n + (p[1 + n] == '$');
            continue;
        }
        if (p[0] == '-' && p[1] == '-') {
            const char *name = p + 2;
            size_t n = strcspn(name, "= \t$");
            const OptionDef *o = option_long(name, n, error, sizeof(error));
            p = name + n;
            char *value = NULL;
            if (o && option_takes_value(o)) {
                if (*p == '=') ++p;
                else while (*p == ' ' || *p == '\t') ++p;
                size_t v = (o->flags & STRING) ? strcspn(p, "$") : strcspn(p, " \t$");
                value = strndup(p, v);
                p += v;
            } else if (*p == '=') p += strcspn(p, " \t$");
            if (*p == '$') ++p;
            if (o) apply_quietly(o, value, HOW_SET, error, sizeof(error));
            free(value);
            continue;
        }
        if (*p == '-') ++p;
        while (*p && !isspace((unsigned char)*p)) {
            int how = HOW_SET;
            if (*p == '+' && p[1]) { how = HOW_RESET; ++p; }
            const OptionDef *o = option_short((unsigned char)*p++);
            if (!o) continue;
            char *value = NULL;
            if (option_takes_value(o)) {
                if (!(o->flags & OPTIONAL)) while (*p == ' ' || *p == '\t') ++p;
                size_t v = short_value(o, p, true);
                value = strndup(p, v);
                p += v;
                if (*p == '$') ++p;
            }
            apply_quietly(o, value, how, error, sizeof(error));
            free(value);
            if (o->flags & STRING) break;
        }
    }
}
int options_parse_argv(int argc, char **argv, char **files, int *file_count, char *error, size_t cap) {
    bool positional = false, help = false, version = false;
    *file_count = 0;
    for (int i = 1; i < argc; ++i) {
        char *a = argv[i];
        if (positional || a[0] == 0 || (a[0] != '-' && a[0] != '+') || !strcmp(a, "-")) {
            files[(*file_count)++] = a;
            continue;
        }
        if (!strcmp(a, "--")) { positional = true; continue; }
        if (a[0] == '+') {
            if (a[1] == '+') add_command(&every_command, "", a + 2);
            else add_command(&first_command, "", a + 1);
            continue;
        }
        if (a[1] == '-') {
            const char *name = a + 2;
            size_t n = strcspn(name, "=");
            const OptionDef *o = option_long(name, n, error, cap);
            if (!o) return -1;
            const char *value = name[n] == '=' ? name + n + 1 : NULL;
            if (option_takes_value(o) && !value && !(o->flags & OPTIONAL)) {
                if (i + 1 >= argc) { snprintf(error, cap, "--%s needs a value", o->name); return -1; }
                value = argv[++i];
            }
            if (o->func == f_help) help = true;
            if (o->func == f_version) version = true;
            if (!option_apply(o, value, HOW_SET, error, cap)) return -1;
            continue;
        }
        /* A bare -h shows help; -hN is less's back-scroll limit. */
        if (!strcmp(a, "-h")) { help = true; continue; }
        for (const char *p = a + 1; *p;) {
            int how = HOW_SET;
            if (*p == '+' && p[1]) { how = HOW_RESET; ++p; }
            char letter = *p++;
            const OptionDef *o = option_short((unsigned char)letter);
            if (!o) { snprintf(error, cap, "There is no -%c option", letter); return -1; }
            const char *value = NULL;
            char *owned = NULL;
            if (option_takes_value(o)) {
                size_t v = short_value(o, p, false);
                if (v) { owned = strndup(p, v); value = owned; p += v; }
                else if (!(o->flags & OPTIONAL)) {
                    if (i + 1 >= argc) { snprintf(error, cap, "-%c needs a value", letter); return -1; }
                    value = argv[++i];
                }
            }
            if (o->func == f_help) help = true;
            if (o->func == f_version) version = true;
            bool ok = option_apply(o, value, how, error, cap);
            free(owned);
            if (!ok) return -1;
        }
    }
    if (help) return -3;
    if (version) return -4;
    return 0;
}
