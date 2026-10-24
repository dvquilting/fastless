#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "pager.h"
#include "search.h"
#include "render.h"
#include "keys.h"
#include "options.h"
#include "input.h"
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

#define MAX_ROWS 1024

/* ---- Terminal ---- */
static struct termios saved_term;
static bool raw_mode, mouse_on;
static int rows = 24, cols = 80;
static void dimensions(void) {
    struct winsize w;
    rows = 24; cols = 80;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)) {
        if (w.ws_row >= 3) rows = w.ws_row;
        if (w.ws_col >= 10) cols = w.ws_col;
    }
    if (rows > MAX_ROWS) rows = MAX_ROWS;
}
static void set_mouse(bool on) {
    if (on == mouse_on) return;
    fputs(on ? "\033[?1000h\033[?1006h" : "\033[?1006l\033[?1000l", stdout);
    mouse_on = on;
}
static void enter_screen(void) {
    struct termios term = saved_term;
    /* IEXTEN off keeps Ctrl-V and Ctrl-O for the pager on BSD and macOS. */
    term.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN);
    term.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    term.c_cc[VMIN] = 1; term.c_cc[VTIME] = 0;
#ifdef VDSUSP
    term.c_cc[VDSUSP] = _POSIX_VDISABLE; /* Ctrl-Y scrolls instead of suspending */
#endif
    tcsetattr(tty_fd, TCSAFLUSH, &term);
    raw_mode = true;
    if (!opt.no_init) fputs("\033[?1049h", stdout);
    set_mouse(opt.mouse);
    fflush(stdout);
}
static void leave_screen(void) {
    if (!raw_mode) return;
    set_mouse(false);
    if (!opt.no_init) fputs("\033[?1049l", stdout);
    else printf("\033[%d;1H\r\033[K", rows);
    fflush(stdout);
    tcsetattr(tty_fd, TCSAFLUSH, &saved_term);
    raw_mode = false;
}
static void restore_at_exit(void) { leave_screen(); }
static void bell(bool movement) {
    if (opt.quiet == 2 || (movement && opt.quiet == 1)) return;
    fputs("\a", stdout);
}

/* ---- Files ---- */
typedef struct {
    char *name;
    InputFile in;
    bool open, is_stdin, seen;
    size_t top;
    int shift;
    char *replacement; /* LESSOPEN replacement file, given to LESSCLOSE */
    LineCache *lines;  /* line counts kept while another file is current */
} File;
static File *files;
static int file_count, current = -1, previous_file = -1;
static InputFile stdin_input;
static bool stdin_ready, secure, any_opened, no_preprocessor;
static char message[1024];
static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
}
/* Set when output is not a terminal: streams are copied, not kept. */
static bool cat_mode;
static InputConfig input_config(void) {
    return (InputConfig){opt.compression, opt.temp_dir, opt.max_memory, opt.threads, opt.force, &interrupted, false,
                         cat_mode};
}
static bool has_magic(const char *name) {
    int fd = open(name, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    unsigned char b[10] = {0};
    ssize_t n = !fstat(fd, &st) && S_ISREG(st.st_mode) ? read(fd, b, sizeof(b)) : 0;
    close(fd);
    return n > 0 && input_detect(b, (size_t)n) != COMP_NONE;
}
static char *shell_quote(const char *s) {
    size_t n = 3;
    for (const char *p = s; *p; ++p) n += *p == '\'' ? 4 : 1;
    char *out = malloc(n), *o = out;
    if (!out) return NULL;
    *o++ = '\'';
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') { memcpy(o, "'\\''", 4); o += 4; }
        else *o++ = *p;
    }
    *o++ = '\''; *o = 0;
    return out;
}
/* Replaces each %s in a LESSOPEN/LESSCLOSE command with the next name. */
static char *substitute(const char *cmd, const char *first, const char *second) {
    size_t cap = strlen(cmd) + 1, len = 0;
    char *out = malloc(cap);
    const char *names[2] = {first, second};
    int used = 0;
    if (!out) return NULL;
    for (const char *p = cmd; *p; ++p) {
        char *q = NULL;
        const char *piece = p;
        size_t n = 1;
        if (p[0] == '%' && p[1] == 's' && used < 2 && names[used]) {
            q = shell_quote(names[used++]);
            piece = q; n = q ? strlen(q) : 0; ++p;
        } else if (p[0] == '%' && p[1] == '%') ++p;
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            char *grown = realloc(out, cap);
            if (!grown) { free(q); free(out); return NULL; }
            out = grown;
        }
        memcpy(out + len, piece, n);
        len += n;
        free(q);
    }
    out[len] = 0;
    return out;
}
static const char *shell_path(void) {
    const char *shell = getenv("SHELL");
    return shell && *shell ? shell : "/bin/sh";
}
static void child_defaults(void) {
    int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH, SIGTSTP, SIGPIPE, SIGQUIT};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) signal(signals[i], SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}
/* Runs cmd with its output read as a document; *status is its wait status. */
static int run_preprocessor(const char *cmd, const char *name, InputFile *out, int *status,
                            char *error, size_t cap) {
    int fds[2];
    if (pipe(fds)) { snprintf(error, cap, "pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); snprintf(error, cap, "fork: %s", strerror(errno)); return -1; }
    if (!pid) {
        child_defaults();
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    InputConfig c = input_config();
    c.compression = COMP_NONE;
    int rc = input_open_fd(out, fds[0], name, &c, false, error, cap);
    close(fds[0]);
    while (waitpid(pid, status, 0) < 0 && errno == EINTR) {}
    return rc;
}
static char *run_for_name(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char buf[PATH_MAX + 2] = "";
    if (!fgets(buf, sizeof(buf), p)) buf[0] = 0;
    pclose(p);
    buf[strcspn(buf, "\r\n")] = 0;
    return buf[0] ? strdup(buf) : NULL;
}
/* Opens a named document through LESSOPEN when it applies. Files fastless
   decompresses itself skip the preprocessor, whose pipe would be slower. */
static int open_document(const char *name, bool is_stdin, InputFile *in, char **replacement,
                         char *error, size_t cap) {
    InputConfig c = input_config();
    *replacement = NULL;
    if (is_stdin) {
        if (!stdin_ready) { snprintf(error, cap, "Standard input is not available"); return -1; }
        *in = stdin_input;
        in->fd = -1;
        return 0;
    }
    const char *lessopen = getenv("LESSOPEN");
    if (lessopen && *lessopen && !opt.no_lessopen && !secure && !no_preprocessor &&
        !(opt.compression == COMP_AUTO && has_magic(name))) {
        bool piped = *lessopen == '|', both = piped && lessopen[1] == '|';
        const char *cmd = lessopen + (both ? 2 : piped ? 1 : 0);
        if (*cmd == '-') ++cmd;
        char *full = substitute(cmd, name, NULL);
        if (full && piped) {
            int status = 0;
            InputFile got;
            int rc = run_preprocessor(full, name, &got, &status, error, cap);
            bool ok_status = WIFEXITED(status) && !WEXITSTATUS(status);
            if (!rc && (got.size || (both && ok_status))) { free(full); *in = got; return 0; }
            if (!rc) input_close(&got);
            if (rc && interrupted) { free(full); return -1; }
            if (opt.show_preproc_errors && !ok_status)
                say("Input preprocessor failed (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        } else if (full) {
            char *name2 = run_for_name(full);
            if (name2 && !input_open(in, name2, &c, error, cap)) {
                free(full);
                *replacement = name2;
                return 0;
            }
            free(name2);
        }
        free(full);
    }
    return input_open(in, name, &c, error, cap);
}
static void close_document(File *f) {
    if (!f->open) return;
    if (!f->is_stdin) input_close(&f->in);
    f->open = false;
    if (f->replacement) {
        const char *lessclose = getenv("LESSCLOSE");
        if (lessclose && *lessclose && !secure) {
            char *cmd = substitute(lessclose, f->name, f->replacement);
            if (cmd) {
                int rc = system(cmd);
                (void)rc;
                free(cmd);
            }
        }
        free(f->replacement);
        f->replacement = NULL;
    }
}

/* ---- View state ---- */
static size_t top;          /* first body line */
static int shift;           /* columns hidden left of the text */
static int header_rows;
static size_t header_begin, header_end;
static Search filter;
static bool filter_on, filter_invert;
static size_t row_pos[MAX_ROWS];
static size_t page_end = SIZE_MAX;  /* line after the last body row, SIZE_MAX at EOF */
static bool new_file_prompt, viewing_help;
static size_t unread_line = SIZE_MAX;
static int half_window;
static const Search *preview;       /* --incsearch pattern being typed */
typedef struct { bool invert, multi, first, keep, no_regex, wrap; int sub; } Flags;
static Search search;
static char *search_text;
static Flags search_flags;
static int search_dir = 1;
static bool hilite_off;
static size_t last_hit, last_hit_end;
static bool have_hit;
static int exit_code;
static bool quitting;
/* Whether the current document is a stream still being read. */
static bool live(void) { return !viewing_help && current >= 0 && files[current].in.reading; }

static int body_rows(void) { int n = rows - 1 - header_rows; return n < 1 ? 1 : n; }
static size_t body_start(void) { return header_rows ? header_end : 0; }
static int status_cols(void) { return opt.status_column ? (opt.status_width > 0 ? opt.status_width : 0) : 0; }
static int number_cols; /* the -N field and its space, 0 without -N */
static int text_width(void) { int w = cols - 1 - status_cols() - number_cols; return w < 1 ? 1 : w; }
static int window_lines(void) {
    int b = body_rows();
    int w = opt.window > 0 ? opt.window : opt.window < 0 ? b + opt.window : b;
    return w < 1 ? 1 : w;
}
static int target_row(void) {
    int b = body_rows();
    int r = opt.jump_is_fraction ? (int)(opt.jump_fraction * b) + 1 :
            opt.jump_line > 0 ? opt.jump_line : b + opt.jump_line + 1;
    return r < 1 ? 1 : r > b ? b : r;
}
static int shift_step(void) {
    int w = text_width() - opt.header_cols;
    int n = opt.shift_is_fraction ? (int)(opt.shift_fraction * w) : opt.shift ? opt.shift : w / 2;
    return n < 1 ? 1 : n;
}
static void compute_header(void) {
    header_rows = 0;
    header_begin = header_end = 0;
    if (!opt.header_lines || viewing_help) return;
    size_t start = 0;
    if (opt.header_start > 1 && line_position(opt.header_start, &start) < 0) start = 0;
    size_t p = start;
    int limit = rows - 2 < opt.header_lines ? rows - 2 : opt.header_lines;
    while (header_rows < limit && p < size) { p = next_line(p); ++header_rows; }
    header_begin = start;
    header_end = p;
}

/* ---- Display lines: what is shown after filtering and squeezing ---- */
static bool blank_line(size_t p) {
    return p < size && (data[p] == '\n' || (data[p] == '\r' && p + 1 < size && data[p + 1] == '\n'));
}
static int shown(size_t p) {
    if (!filter_on) return 1;
    int rc = line_matches(&filter, p, line_end(p));
    return rc < 0 ? -1 : filter_invert ? !rc : rc;
}
static int next_shown(size_t from, size_t *out) {
    if (from >= size) return 0;
    if (!filter_on) { *out = from; return 1; }
    char error[8];
    size_t hit;
    int rc = filter_invert ? nonmatch_forward(&filter, from, &hit, error, sizeof(error))
                           : search_forward(&filter, from, size, &hit, error, sizeof(error));
    if (rc <= 0) return rc;
    *out = line_start(hit);
    return 1;
}
static int prev_shown(size_t before, size_t *out) {
    if (before <= body_start()) return 0;
    size_t line;
    if (!filter_on) line = previous_line(before);
    else {
        char error[8];
        size_t hit;
        int rc = filter_invert ? nonmatch_backward(&filter, before, &hit, error, sizeof(error))
                               : search_backward(&filter, before, &hit, error, sizeof(error));
        if (rc <= 0) return rc;
        line = line_start(hit);
    }
    if (line < body_start()) return 0;
    *out = line;
    return 1;
}
/* The display line after (or before) the display line p: 1, 0 at the end,
   -1 if interrupted. */
static int fwd(size_t p, size_t *out) {
    size_t q = next_line(p);
    for (;;) {
        if (q >= size) return 0;
        int rc = next_shown(q, &q);
        if (rc <= 0) return rc;
        if (opt.squeeze && blank_line(p) && blank_line(q)) { p = q; q = next_line(q); continue; }
        *out = q;
        return 1;
    }
}
static int back(size_t p, size_t *out) {
    size_t q;
    int rc = prev_shown(p, &q);
    if (rc <= 0) return rc;
    while (opt.squeeze && blank_line(q)) {
        size_t r;
        rc = prev_shown(q, &r);
        if (rc < 0) return -1;
        if (!rc || !blank_line(r)) break;
        q = r;
    }
    *out = q;
    return 1;
}
/* The display line holding (or nearest to) an arbitrary position. */
static size_t settle(size_t p) {
    if (p >= size) p = size ? line_start(size - 1) : 0;
    p = line_start(p);
    if (p < body_start()) p = body_start();
    if (filter_on && shown(p) == 0) {
        size_t q;
        if (next_shown(p, &q) == 1 || prev_shown(p, &q) == 1) p = q;
    }
    while (opt.squeeze && blank_line(p) && p > body_start() && blank_line(previous_line(p)))
        p = previous_line(p);
    return p;
}
static size_t back_lines(size_t p, int n) {
    for (int i = 0; i < n; ++i) {
        size_t q;
        if (back(p, &q) != 1) break;
        p = q;
    }
    return p;
}
/* Recomputes page_end and row_pos for the current top without drawing. */
static void layout_rows(void) {
    size_t p = top;
    bool more = top < size;
    for (int r = 0; r < body_rows(); ++r) {
        row_pos[r] = more ? p : SIZE_MAX;
        size_t q;
        if (more) {
            int rc = fwd(p, &q);
            if (rc == 1) p = q;
            else { more = false; if (rc < 0) say("Interrupted"); }
        }
    }
    page_end = more ? p : SIZE_MAX;
}
static size_t bottom_line(void) {
    for (int r = body_rows() - 1; r >= 0; --r)
        if (row_pos[r] != SIZE_MAX) return row_pos[r];
    return top;
}

/* ---- Prompts in less's prompt language ---- */
static const char *default_prompts[PROMPT_COUNT] = {
    "?n?f%f .?m(%T %i of %m) ..?e(END) ?x- Next\\: %x..%t",
    "?n?f%f .?m(%T %i of %m) ..?e(END) ?x- Next\\: %x.:?pB%pB\\%:byte %bB?s/%s...%t",
    "?f%f .?n?m(%T %i of %m) ..?ltlines %lt-%lb?L/%L. :byte %bB?s/%s. .?e(END) ?x- Next\\: %x.:?pB%pB\\%..%t",
    "?f%f .?m(%T %i of %m) .?ltlines %lt-%lb?L/%L. .byte %bB?s/%s. ?e(END) :?pB%pB\\%..%t",
    "Help (press q when done)",
    "Waiting for data... (interrupt to abort)"};
static const char *display_name(void) {
    if (viewing_help) return "HELP";
    if (current < 0) return "";
    return files[current].is_stdin ? "Standard input" : files[current].name;
}
static size_t position_at(char where) {
    switch (where) {
    case 'b': return bottom_line();
    case 'B': return page_end == SIZE_MAX ? size : page_end;
    case 'm': { size_t p = row_pos[body_rows() / 2]; return p == SIZE_MAX ? bottom_line() : p; }
    case 'j': { size_t p = row_pos[target_row() - 1]; return p == SIZE_MAX ? bottom_line() : p; }
    default: return top;
    }
}
typedef struct { char *out; size_t len, cap; } Text;
static void put(Text *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) n = t->cap - t->len - 1;
    memcpy(t->out + t->len, s, n);
    t->len += n;
    t->out[t->len] = 0;
}
static void puts_text(Text *t, const char *s) { put(t, s, strlen(s)); }
/* Line numbers for prompts, when known without waiting. */
static bool line_at(char where, size_t *n) {
    return opt.line_numbers != 1 && line_number_known(position_at(where), n) == 1;
}
static bool total_lines(size_t *n) {
    size_t end;
    if (opt.line_numbers == 1 || line_number_known(size, &end) != 1) return false;
    *n = size ? end - (data[size - 1] == '\n') : 0;
    return true;
}
static bool condition(char c, char where) {
    size_t n, total;
    switch (c) {
    case 'l': case 'd': return line_at(where, &n);
    /* As in less, sizes are unknown until a stream has been read. */
    case 'L': case 'D': return !live() && total_lines(&total);
    case 'P': return !live() && line_at(where, &n) && total_lines(&total) && total;
    case 'a': return true;
    case 'b': return true;
    case 'B': case 'p': case 's': return !live();
    case 'c': return shift > 0;
    case 'e': return page_end == SIZE_MAX && !live();
    case 'f': return !viewing_help && current >= 0 && !files[current].is_stdin;
    case 'm': return file_count > 1 && !viewing_help;
    case 'n': return new_file_prompt;
    case 'x': return !viewing_help && current + 1 < file_count;
    default: return false;
    }
}
static void expand(Text *t, const char **pp, char stop);
static void skip_part(const char **pp) {
    /* Skips to the ':' or '.' ending this branch, minding nesting. */
    int depth = 0;
    for (const char *p = *pp; *p; ++p) {
        if (*p == '\\' && p[1]) { ++p; continue; }
        if (*p == '%' && p[1]) { ++p; if (strchr("bdlpP", *p) && p[1]) ++p; continue; }
        if (*p == '?') { ++depth; if (p[1]) ++p; if (strchr("bdlpP", *p) && p[1]) ++p; continue; }
        if (depth == 0 && (*p == ':' || *p == '.')) { *pp = p; return; }
        if (*p == '.' && depth) --depth;
    }
    *pp += strlen(*pp);
}
static void expand(Text *t, const char **pp, char stop) {
    const char *p = *pp;
    char num[64];
    while (*p && !(stop && (*p == ':' || *p == '.'))) {
        char c = *p++;
        if (c == '\\' && *p) { put(t, p++, 1); continue; }
        if (c == '?' && *p) {
            char cond = *p++;
            char where = 't';
            if (strchr("bdlpP", cond) && *p && strchr("tmbBj", *p)) where = *p++;
            bool truth = cond == 'a' ? t->len > 0 : condition(cond, where);
            if (truth) {
                expand(t, &p, 1);
                if (*p == ':') { ++p; skip_part(&p); }
            } else {
                skip_part(&p);
                if (*p == ':') { ++p; expand(t, &p, 1); }
            }
            if (*p == '.') ++p;
            continue;
        }
        if (c != '%' || !*p) { put(t, &c, 1); continue; }
        char k = *p++;
        char where = 't';
        if (strchr("bdlpP", k) && *p && strchr("tmbBj", *p)) where = *p++;
        switch (k) {
        case 'b': snprintf(num, sizeof(num), "%zu", position_at(where)); puts_text(t, num); break;
        case 'B': case 's': snprintf(num, sizeof(num), "%zu", size); puts_text(t, num); break;
        case 'c': snprintf(num, sizeof(num), "%d", shift + 1); puts_text(t, num); break;
        case 'E': {
            /* As in less, an empty variable counts as unset. */
            const char *e = getenv("VISUAL");
            if (!e || !*e) e = getenv("EDITOR");
            puts_text(t, e && *e ? e : "vi");
            break;
        }
        case 'f': puts_text(t, display_name()); break;
        case 'F': { const char *n = display_name(), *s = strrchr(n, '/'); puts_text(t, s ? s + 1 : n); break; }
        case 'g': { char *q = shell_quote(display_name()); if (q) { puts_text(t, q); free(q); } break; }
        case 'i': snprintf(num, sizeof(num), "%d", current + 1); puts_text(t, num); break;
        case 'm': snprintf(num, sizeof(num), "%d", file_count); puts_text(t, num); break;
        case 'p': {
            size_t pos = position_at(where);
            snprintf(num, sizeof(num), "%d", size ? (int)(100.0 * (double)pos / (double)size) : 100);
            puts_text(t, num);
            break;
        }
        case 't': while (t->len && t->out[t->len - 1] == ' ') t->out[--t->len] = 0; break;
        case 'T': puts_text(t, "file"); break;
        case 'x': if (current + 1 < file_count) puts_text(t, files[current + 1].name); break;
        case 'l': case 'd': {
            size_t n;
            if (!line_at(where, &n)) { puts_text(t, "?"); break; }
            if (k == 'd') n = (n - 1) / (size_t)body_rows() + 1;
            snprintf(num, sizeof(num), "%zu", n);
            puts_text(t, num);
            break;
        }
        case 'L': case 'D': {
            size_t total;
            if (!total_lines(&total)) { puts_text(t, "?"); break; }
            if (k == 'D') total = total ? (total - 1) / (size_t)body_rows() + 1 : 0;
            snprintf(num, sizeof(num), "%zu", total);
            puts_text(t, num);
            break;
        }
        case 'P': {
            size_t n, total;
            if (!line_at(where, &n) || !total_lines(&total) || !total) { puts_text(t, "?"); break; }
            snprintf(num, sizeof(num), "%d", (int)(100.0 * (double)n / (double)total));
            puts_text(t, num);
            break;
        }
        default: put(t, &k, 1); break;
        }
    }
    *pp = p;
}
static void expand_prompt(const char *proto, char *out, size_t cap) {
    Text t = {out, 0, cap};
    out[0] = 0;
    const char *p = proto;
    while (*p) {
        expand(&t, &p, 0);
        if (*p) put(&t, p++, 1);
    }
}

/* ---- Drawing ---- */
static void put_clipped(const char *s, int width) {
    size_t n = strlen(s), keep = 0;
    while (keep < n && render_text(NULL, s, keep + 1) <= width) ++keep;
    /* Do not split a UTF-8 sequence. */
    while (keep < n && ((unsigned char)s[keep] & 0xC0) == 0x80) --keep;
    render_text(stdout, s, keep);
}
static bool has_mark(size_t begin, size_t end, char *letter);
static const Search *highlighting(LineRequest *r) {
    if (preview) return preview;
    if (!search_text || hilite_off || opt.hilite == 2 || search_flags.invert) return NULL;
    if (opt.hilite == 1) {
        if (have_hit) { r->only_from = last_hit; r->only_to = last_hit_end; }
        return NULL;
    }
    return &search;
}
/* -N: the line number of each body row (0 while unknown) and of the first
   header line. Rows are numbered from their neighbours, so a screen costs
   one cached lookup plus the newlines between its rows. */
static size_t row_line[MAX_ROWS], header_line;
static bool numbers_shown(void) { return opt.line_numbers == 2; }
static int digits(size_t n) { int d = 1; while (n >= 10) { n /= 10; ++d; } return d; }
static void number_rows(void) {
    number_cols = 0;
    if (!numbers_shown()) return;
    size_t widest = 0;
    for (int r = 0; r < body_rows(); ++r) {
        size_t p = row_pos[r], n = 0;
        row_line[r] = 0;
        if (p == SIZE_MAX) continue;
        if (r && row_line[r - 1] && p - row_pos[r - 1] <= ((size_t)1 << 20))
            n = row_line[r - 1] + literal_count(data + row_pos[r - 1], p - row_pos[r - 1], '\n');
        else if (line_number_known(p, &n) != 1) n = 0;
        row_line[r] = n;
        if (n > widest) widest = n;
    }
    header_line = 0;
    if (header_rows && !opt.no_number_headers && line_number_known(header_begin, &header_line) == 1 &&
        header_line + (size_t)header_rows > widest)
        widest = header_line + (size_t)header_rows;
    int w = opt.line_num_width > 0 ? opt.line_num_width : 1;
    if (digits(widest) > w) w = digits(widest);
    number_cols = w + 1;
}
static void put_number(size_t n) {
    if (!number_cols) return;
    if (n) printf("%*zu ", number_cols - 1, n);
    else printf("%*s", number_cols, "");
}
static void draw_line(size_t p, bool body, size_t number) {
    size_t end = line_end(p);
    int attr = 0;
    char letter = 0;
    bool marked = has_mark(p, end, &letter);
    if (body && p == unread_line) attr = ATTR_REVERSE;
    if (opt.status_line && marked) attr = ATTR_REVERSE;
    if (opt.status_column && opt.status_width > 0) {
        char c = ' ';
        if (marked) c = letter;
        else if (search_text && !hilite_off && opt.hilite != 2 && line_matches(&search, p, end) == 1) c = '*';
        putchar(c);
        for (int i = 1; i < opt.status_width; ++i) putchar(' ');
    }
    put_number(number);
    LineRequest r = {.begin = p, .end = end, .header_cols = opt.header_cols, .shift = shift,
                     .width = text_width(), .line_attr = attr, .rscroll = opt.rscroll};
    r.search = highlighting(&r);
    render_line(stdout, &r);
}
static void status_line(void) {
    char text[1024];
    if (opt.prompt_style || opt.prompt[PROMPT_SHORT] || viewing_help) {
        if (message[0]) snprintf(text, sizeof(text), "%.1000s", message);
        else {
            int which = viewing_help ? PROMPT_HELP : opt.prompt_style == 2 ? PROMPT_LONG :
                        opt.prompt_style == 1 ? PROMPT_MEDIUM : PROMPT_SHORT;
            const char *proto = opt.prompt[which] ? opt.prompt[which] : default_prompts[which];
            expand_prompt(proto, text, sizeof(text));
            if (!text[0]) snprintf(text, sizeof(text), ":");
        }
    } else {
        char where[512] = "";
        if (file_count > 1) snprintf(where, sizeof(where), "%s (file %d of %d) | ", display_name(), current + 1, file_count);
        const char *mode = search.mode == REGEX ? "regex" : search.mode == LITERAL ? "literal" : "glob";
        if (!search_text) mode = opt.mode == 0 ? "regex" : opt.mode == 1 ? "literal" : "glob";
        const char *activity = live() ? input_activity(&files[current].in) : NULL;
        char at[128];
        if (activity) snprintf(at, sizeof(at), "byte %zu/%zu (%s)", top, size, activity);
        else snprintf(at, sizeof(at), "byte %zu/%zu (%.1f%%)%s", top, size,
                      size ? 100.0 * (double)top / (double)size : 100.0, page_end == SIZE_MAX ? " (END)" : "");
        snprintf(text, sizeof(text), "fastless | %.400s%s | %s%s%s | %.400s",
                 where, at, mode,
                 (search_text ? search.fold : opt.case_mode == 2) ? ", ignore case" : "",
                 filter_on ? ", filtered" : "", message[0] ? message : "q quit; h help; / search");
    }
    fputs("\033[7m\033[2K", stdout);
    const char *shown_text = text;
    /* As in less, a long prompt keeps its end, where the position is. */
    if (opt.prompt_style || opt.prompt[PROMPT_SHORT] || viewing_help)
        while (*shown_text && render_text(NULL, shown_text, strlen(shown_text)) > cols - 1) ++shown_text;
    put_clipped(shown_text, cols - 1);
    fputs("\033[0m", stdout);
}
static void draw(void) {
    dimensions();
    layout_rows();
    number_rows();
    if (opt.clear) fputs("\033[2J", stdout);
    fputs("\033[0m\033[H", stdout);
    size_t hp = header_begin;
    for (int r = 0; r < header_rows; ++r) {
        fputs("\033[2K", stdout);
        if (hp < size) { draw_line(hp, false, header_line ? header_line + (size_t)r : 0); hp = next_line(hp); }
        fputs("\r\n", stdout);
    }
    for (int r = 0; r < body_rows(); ++r) {
        fputs("\033[2K", stdout);
        if (row_pos[r] != SIZE_MAX) draw_line(row_pos[r], true, numbers_shown() ? row_line[r] : 0);
        else if (!opt.tilde) putchar('~');
        fputs("\r\n", stdout);
    }
    status_line();
    fflush(stdout);
}
/* Prints document lines without the pager, for -F and --redraw-on-quit. */
static void print_lines(size_t from, int count) {
    size_t p = from, n = 0;
    number_cols = 0;
    if (numbers_shown() && line_number(from, &n) == 1)
        number_cols = (opt.line_num_width > 0 ? opt.line_num_width : 1) + 1;
    for (int i = 0; i < count && p < size; ++i) {
        put_number(n);
        LineRequest r = {.begin = p, .end = line_end(p), .header_cols = opt.header_cols,
                         .width = text_width() + status_cols(), .rscroll = opt.rscroll};
        render_line(stdout, &r);
        putchar('\n');
        size_t q;
        if (fwd(p, &q) != 1) break;
        if (n) n += literal_count(data + p, q - p, '\n');
        p = q;
    }
}

/* ---- Counting lines ---- */
/* Counts synchronously for an explicit request such as =; Ctrl-C leaves
   whatever is known. */
static void count_lines_now(size_t pos) {
    size_t n;
    if (opt.line_numbers == 1 || !size || line_number_known(pos, &n) == 1) return;
    printf("\033[%d;1H\033[0m\033[2KCalculating line numbers... (interrupt to abort)", rows);
    fflush(stdout);
    if (line_number(pos, &n) < 0) interrupted = 0;
}
static const char *active_prompt(void) {
    if (!(opt.prompt_style || opt.prompt[PROMPT_SHORT] || viewing_help)) return NULL;
    int which = viewing_help ? PROMPT_HELP : opt.prompt_style == 2 ? PROMPT_LONG :
                opt.prompt_style == 1 ? PROMPT_MEDIUM : PROMPT_SHORT;
    return opt.prompt[which] ? opt.prompt[which] : default_prompts[which];
}
static bool prompt_mentions(const char *letters) {
    const char *proto = active_prompt();
    for (const char *p = proto ? proto : ""; *p; ++p)
        if ((*p == '%' || *p == '?') && p[1] && strchr(letters, p[1])) return true;
    return false;
}
/* How far line numbers are needed: the screen for -N and %l, the whole
   file for %L, %D, and %P; SIZE_MAX when nothing shows them. */
static size_t lines_wanted(void) {
    if (opt.line_numbers == 1 || !size) return SIZE_MAX;
    if (prompt_mentions("LDP")) return size;
    if (numbers_shown() || prompt_mentions("ld")) return page_end == SIZE_MAX ? size : page_end;
    return SIZE_MAX;
}
/* While no key is waiting, counts a few pieces at a time and redraws when
   the numbers on screen become known, so that moving and searching never
   wait for a count. Ctrl-C turns line numbers off, as in less. */
static void count_lines_idle(void) {
    size_t want = lines_wanted(), n;
    if (want == SIZE_MAX || lines_progress(want) >= 1.0) return;
    bool top_known = line_number_known(top, &n) == 1;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    double shown = 0;
    while (!keys_queued() && !keys_waiting() && !resized && !exiting && !suspended) {
        /* 256 MiB at a time keeps a key waiting at most a few tens of
           milliseconds when the file is cached. */
        bool more = lines_count_more(want, (size_t)256 << 20);
        if (interrupted) {
            interrupted = 0;
            opt.line_numbers = 1;
            say("Line numbers turned off");
            draw();
            return;
        }
        if (!more) { draw(); return; }
        if (!top_known && line_number_known(top, &n) == 1) { top_known = true; draw(); }
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed > 0.3 && elapsed - shown > 0.25 && !message[0]) {
            shown = elapsed;
            say("Counting lines: %d%% (Ctrl-C turns line numbers off)", (int)(100 * lines_progress(want)));
            printf("\033[%d;1H", rows);
            status_line();
            fflush(stdout);
            message[0] = 0;
        }
    }
}

/* ---- History and line editing ---- */
enum { H_NONE, H_SEARCH, H_SHELL, H_KINDS };
typedef struct { char **items; int count; } History;
static History history[H_KINDS];
static bool history_changed;
static void history_add(int kind, const char *text) {
    if (kind == H_NONE || !*text) return;
    History *h = &history[kind];
    if (h->count && !strcmp(h->items[h->count - 1], text)) return;
    if (opt.no_histdups)
        for (int i = 0; i < h->count; ++i)
            if (!strcmp(h->items[i], text)) {
                free(h->items[i]);
                memmove(h->items + i, h->items + i + 1, (size_t)(h->count - i - 1) * sizeof(char *));
                --h->count; --i;
            }
    if (h->count == 100) { free(h->items[0]); memmove(h->items, h->items + 1, 99 * sizeof(char *)); --h->count; }
    char **grown = realloc(h->items, (size_t)(h->count + 1) * sizeof(char *));
    char *copy = strdup(text);
    if (!grown || !copy) { free(copy); if (grown) h->items = grown; return; }
    h->items = grown;
    h->items[h->count++] = copy;
    history_changed = true;
}
static char *history_path(void) {
    const char *p = getenv("FASTLESSHISTFILE");
    if (p) return *p && strcmp(p, "-") ? strdup(p) : NULL;
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char *path = malloc(strlen(home) + 16);
    if (path) sprintf(path, "%s/.fastlesshst", home);
    return path;
}
typedef struct { char letter; char *file; size_t pos; int row; bool set; } Mark;
static Mark marks[256];
static void history_load(void) {
    char *path = history_path();
    FILE *f = path ? fopen(path, "r") : NULL;
    free(path);
    if (!f) return;
    char line[8192];
    int kind = H_NONE;
    bool mark_section = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, ".search")) { kind = H_SEARCH; mark_section = false; }
        else if (!strcmp(line, ".shell")) { kind = H_SHELL; mark_section = false; }
        else if (!strcmp(line, ".mark")) { kind = H_NONE; mark_section = true; }
        else if (line[0] == '"' && kind != H_NONE) {
            bool dups = opt.no_histdups;
            opt.no_histdups = 0;
            history_add(kind, line + 1);
            opt.no_histdups = dups;
        } else if (mark_section && opt.save_marks && line[0] == 'm' && line[1] == ' ') {
            char letter;
            int row;
            unsigned long long pos;
            int used = 0;
            if (sscanf(line, "m %c %d %llu %n", &letter, &row, &pos, &used) == 3 && used &&
                isalpha((unsigned char)letter)) {
                Mark *m = &marks[(unsigned char)letter];
                free(m->file);
                *m = (Mark){letter, strdup(line + used), (size_t)pos, row, true};
            }
        }
    }
    fclose(f);
    history_changed = false;
}
static void history_save(void) {
    if (secure || (!history_changed && !opt.save_marks)) return;
    char *path = history_path();
    if (!path) return;
    char *tmp = malloc(strlen(path) + 8);
    if (!tmp) { free(path); return; }
    sprintf(tmp, "%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (f) {
        fputs(".fastless-history-file:\n", f);
        const char *names[] = {NULL, ".search", ".shell"};
        for (int k = H_SEARCH; k < H_KINDS; ++k) {
            fprintf(f, "%s\n", names[k]);
            for (int i = 0; i < history[k].count; ++i) fprintf(f, "\"%s\n", history[k].items[i]);
        }
        if (opt.save_marks) {
            fputs(".mark\n", f);
            for (int c = 0; c < 256; ++c)
                if (marks[c].set && marks[c].file && isalpha(c))
                    fprintf(f, "m %c %d %zu %s\n", c, marks[c].row, marks[c].pos, marks[c].file);
        }
        if (fclose(f) || rename(tmp, path)) unlink(tmp);
    } else if (fd >= 0) { close(fd); unlink(tmp); }
    free(tmp);
    free(path);
}
typedef struct {
    char *label;           /* shown before the text; may change while typing */
    size_t label_cap;
    int history;
    /* Called for a character typed into the empty line; true if consumed. */
    bool (*modifier)(int c, char *label, size_t cap);
    void (*changed)(const char *text);
} PromptSpec;
static size_t utf8_back(const char *s, size_t i) {
    if (!i) return 0;
    --i;
    while (i && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return i;
}
static size_t utf8_forward(const char *s, size_t len, size_t i) {
    if (i >= len) return len;
    ++i;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
    return i;
}
static bool word_char(char c) { return c != ' ' && c != '\t'; }
static void prompt_draw(const PromptSpec *spec, const char *text, size_t len, size_t cursor) {
    int label = render_text(NULL, spec->label, strlen(spec->label));
    int room = cols - 1 - label;
    if (room < 4) room = 4;
    /* Scroll so the cursor stays visible. */
    size_t from = 0;
    while (from < cursor && render_text(NULL, text + from, cursor - from) >= room) from = utf8_forward(text, len, from);
    printf("\033[%d;1H\033[0m\033[2K", rows);
    render_text(stdout, spec->label, strlen(spec->label));
    size_t to = from;
    while (to < len && render_text(NULL, text + from, utf8_forward(text, len, to) - from) < room)
        to = utf8_forward(text, len, to);
    render_text(stdout, text + from, to - from);
    int col = label + render_text(NULL, text + from, cursor - from) + 1;
    printf("\033[%d;%dH", rows, col > cols ? cols : col);
    fflush(stdout);
}
/* Reads a line with less's editing keys. False if cancelled. */
static bool prompt_line(PromptSpec *spec, char *out, size_t cap) {
    size_t len = strlen(out), cursor = len;
    History *h = &history[spec->history];
    int hist = h->count;
    char saved[4096] = "";
    for (;;) {
        prompt_draw(spec, out, len, cursor);
        int c = 0;
        EditAction e = read_edit(&c);
        if (e == E_ABORT && c == KEY_SIGNAL && !interrupted && !exiting) {
            if (suspended) return false;
            resized = 0;
            dimensions();
            draw();
            continue;
        }
        bool edited = true;
        switch (e) {
        case E_ABORT: return false;
        case E_IGNORE: edited = false; break;
        case E_LITERAL:
            c = key_read(-1);
            if (c < 0) return false;
            /* fall through */
        case E_INSERT:
            if (e == E_INSERT && (c == '\r' || c == '\n')) {
                out[len] = 0;
                history_add(spec->history, out);
                return true;
            }
            if (e == E_INSERT && !len && spec->modifier && spec->modifier(c, spec->label, spec->label_cap)) {
                edited = false;
                break;
            }
            if (e == E_INSERT && (c < 0x20 || c == 0x7f)) { edited = false; break; }
            if (len + 1 < cap) {
                memmove(out + cursor + 1, out + cursor, len - cursor);
                out[cursor++] = (char)c;
                ++len;
            }
            break;
        case E_BACKSPACE:
            if (!len) return false; /* as in less, backspacing past the start cancels */
            if (cursor) {
                size_t from = utf8_back(out, cursor);
                memmove(out + from, out + cursor, len - cursor);
                len -= cursor - from;
                cursor = from;
            }
            break;
        case E_DELETE:
            if (cursor < len) {
                size_t to = utf8_forward(out, len, cursor);
                memmove(out + cursor, out + to, len - to);
                len -= to - cursor;
            }
            break;
        case E_WORD_BACKSPACE: {
            size_t from = cursor;
            while (from && !word_char(out[from - 1])) --from;
            while (from && word_char(out[from - 1])) --from;
            memmove(out + from, out + cursor, len - cursor);
            len -= cursor - from;
            cursor = from;
            break;
        }
        case E_WORD_DELETE: {
            size_t to = cursor;
            while (to < len && !word_char(out[to])) ++to;
            while (to < len && word_char(out[to])) ++to;
            memmove(out + cursor, out + to, len - to);
            len -= to - cursor;
            break;
        }
        case E_LEFT: cursor = utf8_back(out, cursor); edited = false; break;
        case E_RIGHT: cursor = utf8_forward(out, len, cursor); edited = false; break;
        case E_WORD_LEFT:
            while (cursor && !word_char(out[cursor - 1])) --cursor;
            while (cursor && word_char(out[cursor - 1])) --cursor;
            edited = false;
            break;
        case E_WORD_RIGHT:
            while (cursor < len && word_char(out[cursor])) ++cursor;
            while (cursor < len && !word_char(out[cursor])) ++cursor;
            edited = false;
            break;
        case E_HOME: cursor = 0; edited = false; break;
        case E_END: cursor = len; edited = false; break;
        case E_KILL_LINE: len = cursor = 0; break;
        case E_UP: case E_DOWN: {
            /* Entries starting with what was typed before browsing. */
            if (hist == h->count) { out[len] = 0; snprintf(saved, sizeof(saved), "%s", out); }
            int step = e == E_UP ? -1 : 1, i = hist + step;
            size_t prefix = strlen(saved);
            while (i >= 0 && i < h->count && strncmp(h->items[i], saved, prefix)) i += step;
            if (i < 0) { edited = false; break; }
            hist = i < h->count ? i : h->count;
            snprintf(out, cap, "%s", hist < h->count ? h->items[hist] : saved);
            len = cursor = strlen(out);
            break;
        }
        default: edited = false; break;
        }
        out[len] = 0;
        if (edited && spec->changed) spec->changed(out);
    }
}
static bool simple_prompt(const char *label, char *out, size_t cap, int kind) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", label);
    PromptSpec spec = {.label = buf, .label_cap = sizeof(buf), .history = kind};
    out[0] = 0;
    return prompt_line(&spec, out, cap);
}
static int prompt_key(const char *label) {
    printf("\033[%d;1H\033[0m\033[2K", rows);
    render_text(stdout, label, strlen(label));
    fflush(stdout);
    int c = key_read(-1);
    return c;
}

/* ---- Marks ---- */
static const char *mark_file(void) {
    if (viewing_help || current < 0) return NULL;
    return files[current].name;
}
static bool has_mark(size_t begin, size_t end, char *letter) {
    const char *name = mark_file();
    if (!name) return false;
    for (int c = 'A'; c <= 'z'; ++c) {
        if (!isalpha(c) && c != '#') continue;
        const Mark *m = &marks[c];
        if (m->set && m->file && !strcmp(m->file, name) && m->pos >= begin && m->pos <= end) {
            *letter = (char)c;
            return true;
        }
    }
    const Mark *m = &marks['#'];
    if (m->set && m->file && !strcmp(m->file, name) && m->pos >= begin && m->pos <= end) { *letter = '#'; return true; }
    return false;
}
static void set_mark(int c, size_t pos, int row) {
    const char *name = mark_file();
    if (!name) return;
    Mark *m = &marks[c & 0xff];
    free(m->file);
    *m = (Mark){(char)c, strdup(name), pos, row, true};
}
static void remember_position(void) { set_mark('\'', top, 1); }

/* ---- Opening and switching files ---- */
static void reset_search_position(void) { have_hit = false; }
static void apply_render(void) {
    render.raw_color = opt.raw != 0;
    render.ctl_backspace = opt.proc_backspace == 2 || (opt.proc_backspace == 0 && opt.special >= 1);
    render.ctl_return = opt.proc_return == 2 || (opt.proc_return == 0 && opt.special >= 1);
    render.ctl_tab = opt.proc_tab == 2 || (opt.proc_tab == 0 && opt.special == 2);
}
/* +commands run for the first file, ++commands for every file examined. */
static void queue_commands(void) {
    static bool first_done;
    if (!first_done && first_command) keys_queue(first_command, strlen(first_command), false);
    if (every_command) keys_queue(every_command, strlen(every_command), false);
    first_done = true;
}
static bool open_file(int index, char *error, size_t cap) {
    File *f = &files[index];
    if (f->open) return true;
    if (open_document(f->name, f->is_stdin, &f->in, &f->replacement, error, cap)) return false;
    f->open = true;
    wake_fd = input_wake_fd();
    return true;
}
/* Makes files[index] current. On failure the current file stays. */
static bool switch_file(int index) {
    if (index < 0 || index >= file_count) return false;
    if (index == current && files[index].open) return true;
    char error[512] = "";
    if (raw_mode) { printf("\033[%d;1H\033[0m\033[2KOpening %s...", rows, files[index].name); fflush(stdout); }
    if (!open_file(index, error, sizeof(error))) {
        say("%s", error[0] ? error : "Cannot open file");
        return false;
    }
    if (current >= 0 && current != index) {
        files[current].top = top;
        files[current].shift = shift;
        files[current].lines = lines_detach();
        /* Decoded documents can be large; only the current one is kept.
           Pipes and devices cannot be read again, so they stay open, but
           stop reading beyond what was asked for. */
        if (files[current].in.once || files[current].is_stdin) input_demand(&files[current].in, 0);
        else close_document(&files[current]);
        previous_file = current;
    }
    current = index;
    File *f = &files[index];
    char news[512];
    if (input_update(&f->in, news, sizeof(news)) < 0) say("%s", news);
    data = f->in.data;
    size = f->in.size;
    lines_attach(f->lines, f->in.stream != NULL);
    f->lines = NULL;
    compute_header();
    bool first = !f->seen;
    if (first && !any_opened) f->top = opt.offset;
    top = settle(first && !any_opened ? opt.offset : f->top);
    shift = first ? 0 : f->shift;
    new_file_prompt = true;
    reset_search_position();
    if (first) queue_commands();
    f->seen = true;
    any_opened = true;
    return true;
}
/* Rereads the current file after it may have changed (v, R). */
static void reload_current(void) {
    if (viewing_help || current < 0) return;
    File *f = &files[current];
    if (f->is_stdin || f->in.once) return;
    size_t keep = top;
    int keep_shift = shift;
    close_document(f);
    char error[512];
    if (!open_file(current, error, sizeof(error))) { say("%s", error); data = (const unsigned char *)""; size = 0; return; }
    data = f->in.data;
    size = f->in.size;
    lines_reset();
    compute_header();
    top = settle(keep);
    shift = keep_shift;
}
static int find_file(const char *name) {
    for (int i = 0; i < file_count; ++i)
        if (!strcmp(files[i].name, name)) return i;
    return -1;
}
static bool add_file(int at, const char *name) {
    File *grown = realloc(files, (size_t)(file_count + 1) * sizeof(File));
    if (!grown) return false;
    files = grown;
    memmove(files + at + 1, files + at, (size_t)(file_count - at) * sizeof(File));
    files[at] = (File){.name = strdup(name), .is_stdin = !strcmp(name, "-")};
    files[at].in.fd = -1;
    ++file_count;
    if (current >= at) ++current;
    if (previous_file >= at) ++previous_file;
    return files[at].name != NULL;
}

/* ---- Help ---- */
static const char *const help_lines[] = {
    "                   SUMMARY OF FASTLESS COMMANDS\n",
    "\n",
    "      Commands marked with * may be preceded by a number, N.\n",
    "      A key preceded by a caret is a Ctrl key; ESC-x is Escape then x.\n",
    "\n",
    "  h  H                 Display this help.\n",
    "  q  :q  Q  :Q  ZZ     Exit.\n",
    " ---------------------------------------------------------------------------\n",
    "                           MOVING\n",
    "\n",
    "  e  ^E  j  ^N  CR  *  Forward  one line   (or N lines).\n",
    "  y  ^Y  k  ^K  ^P  *  Backward one line   (or N lines).\n",
    "  f  ^F  ^V  SPACE  *  Forward  one window (or N lines).\n",
    "  b  ^B  ESC-v      *  Backward one window (or N lines).\n",
    "  z                 *  Forward  one window (and set window to N).\n",
    "  w                 *  Backward one window (and set window to N).\n",
    "  ESC-SPACE         *  Forward  one window, but don't stop at end-of-file.\n",
    "  d  ^D             *  Forward  one half-window (and set half-window to N).\n",
    "  u  ^U             *  Backward one half-window (and set half-window to N).\n",
    "  ESC-)  RightArrow *  Right one half screen width (or N positions).\n",
    "  ESC-(  LeftArrow  *  Left  one half screen width (or N positions).\n",
    "  ESC-}  ^RightArrow   Right to last column displayed.\n",
    "  ESC-{  ^LeftArrow    Left  to first column.\n",
    "  F                    Forward forever; like \"tail -f\".\n",
    "  ESC-F                Like F but stop when search pattern is found.\n",
    "  r  ^R  ^L            Repaint screen.\n",
    "  R                    Repaint screen, discarding buffered input.\n",
    " ---------------------------------------------------------------------------\n",
    "                          SEARCHING\n",
    "\n",
    "  /pattern          *  Search forward for (N-th) match.\n",
    "  ?pattern          *  Search backward for (N-th) match.\n",
    "  n                 *  Repeat previous search (for N-th occurrence).\n",
    "  N                 *  Repeat previous search in reverse direction.\n",
    "  ESC-n             *  Repeat previous search, spanning files.\n",
    "  ESC-N             *  Repeat previous search, reverse dir. & spanning files.\n",
    "  ESC-u                Undo (toggle) search highlighting.\n",
    "  ESC-U                Clear search highlighting.\n",
    "  &pattern             Display only matching lines; & alone shows all.\n",
    "  ESC-r  ESC-l  ESC-w  Search with regular expressions, literals, or globs.\n",
    "\n",
    "        A search pattern may begin with one or more of:\n",
    "        ^N or !  Search for NON-matching lines.\n",
    "        ^E or *  Search multiple files (pass thru END OF FILE).\n",
    "        ^F or @  Start search at FIRST file (for /) or last file (for ?).\n",
    "        ^K       Highlight matches, but don't move (KEEP position).\n",
    "        ^R       Don't use REGULAR EXPRESSIONS (literal search).\n",
    "        ^S n     Search for match in n-th parenthesized subpattern.\n",
    "        ^W       WRAP search if no match found.\n",
    "        ^L       Enter next character literally into pattern.\n",
    " ---------------------------------------------------------------------------\n",
    "                           JUMPING\n",
    "\n",
    "  g  <  ESC-<       *  Go to first line in file (or line N).\n",
    "  G  >  ESC->       *  Go to last line in file (or line N).\n",
    "  p  %              *  Go to beginning of file (or N percent into file).\n",
    "  P                 *  Go to byte offset N.\n",
    "  {  (  [           *  Find close bracket } ) ].\n",
    "  }  )  ]           *  Find open bracket { ( [.\n",
    "  ESC-^F <c1> <c2>  *  Find close bracket <c2>.\n",
    "  ESC-^B <c1> <c2>  *  Find open bracket <c1>.\n",
    "  m<letter>            Mark the current top line with <letter>.\n",
    "  M<letter>            Mark the current bottom line with <letter>.\n",
    "  '<letter>            Go to a previously marked position.\n",
    "  ''                   Go to the previous position.\n",
    "  ^X^X                 Same as '.\n",
    "  ESC-m<letter>        Clear a mark.\n",
    "        Marks ^ and $ are the beginning and end of the file.\n",
    "        Line numbers (-N, -M, =) are counted only when shown; a long jump\n",
    "        shows them once counted, without waiting. Ctrl-C turns them off.\n",
    " ---------------------------------------------------------------------------\n",
    "                        CHANGING FILES\n",
    "\n",
    "  :e [file]            Examine a new file.\n",
    "  ^X^V                 Same as :e.\n",
    "  :n                *  Examine the (N-th) next file from the command line.\n",
    "  :p                *  Examine the (N-th) previous file from the command line.\n",
    "  :x                *  Examine the first (or N-th) file from the command line.\n",
    "  :d                   Delete the current file from the command line list.\n",
    "  =  ^G  :f            Print current file name.\n",
    " ---------------------------------------------------------------------------\n",
    "                    MISCELLANEOUS COMMANDS\n",
    "\n",
    "  -<flag>              Toggle a command line option.\n",
    "  --<name>             Toggle a command line option, by name.\n",
    "  _<flag>              Display the setting of a command line option.\n",
    "  __<name>             Display the setting of an option, by name.\n",
    "  +cmd                 Execute the command each time a new file is examined.\n",
    "  !command             Execute the shell command with $SHELL.\n",
    "  #command             Execute the shell command, expanded like a prompt.\n",
    "  |Xcommand            Pipe file between current pos & mark X to shell command.\n",
    "  s file               Save input to a file.\n",
    "  v                    Edit the current file with $VISUAL or $EDITOR.\n",
    "  V                    Print version number.\n",
    " ---------------------------------------------------------------------------\n",
    "                          LINE EDITING\n",
    "\n",
    "  LeftArrow  ESC-h     Move cursor left.     RightArrow  ESC-l  Move right.\n",
    "  ^LeftArrow ESC-b     Word left.            ^RightArrow ESC-w  Word right.\n",
    "  HOME  ESC-0          Start of line.        END  ESC-$         End of line.\n",
    "  BACKSPACE            Delete left.          DELETE  ESC-x      Delete under.\n",
    "  ESC-BACKSPACE        Delete word left.     ESC-X              Delete word.\n",
    "  ^U                   Delete line.          ^V  ^A             Literal next.\n",
    "  UpArrow  ESC-k       Previous command.     DownArrow  ESC-j   Next command.\n",
    "  ESC (alone)  ^G      Cancel.\n"};
static char *help_text;
static size_t help_size;
static size_t help_top;
static int help_shift;
static bool help_filter;
static LineCache *help_saved_lines;
static void show_help(void) {
    if (viewing_help) return;
    help_top = top;
    help_shift = shift;
    help_filter = filter_on;
    filter_on = false;
    if (!help_text) {
        for (size_t i = 0; i < sizeof(help_lines) / sizeof(help_lines[0]); ++i) help_size += strlen(help_lines[i]);
        help_text = malloc(help_size + 1);
        if (!help_text) { help_size = 0; filter_on = help_filter; return; }
        help_text[0] = 0;
        for (size_t i = 0, n = 0; i < sizeof(help_lines) / sizeof(help_lines[0]); ++i) {
            size_t len = strlen(help_lines[i]);
            memcpy(help_text + n, help_lines[i], len + 1);
            n += len;
        }
    }
    viewing_help = true;
    help_saved_lines = lines_detach();
    data = (const unsigned char *)help_text;
    size = help_size;
    compute_header();
    top = 0;
    shift = 0;
}
static void close_help(void) {
    if (!viewing_help) return;
    viewing_help = false;
    filter_on = help_filter;
    char news[512];
    if (input_update(&files[current].in, news, sizeof(news)) < 0) say("%s", news);
    data = files[current].in.data;
    size = files[current].in.size;
    lines_attach(help_saved_lines, files[current].in.stream != NULL);
    help_saved_lines = NULL;
    compute_header();
    top = settle(help_top);
    shift = help_shift;
}

/* ---- Streams being read ---- */
static char stream_failure[512]; /* a read error, until a command reports it */
/* Takes in what has arrived for the current document: true if it changed. */
static bool stream_refresh(void) {
    if (viewing_help || current < 0 || !files[current].in.reading) return false;
    File *f = &files[current];
    const unsigned char *old = data;
    size_t old_size = size;
    char error[512];
    int rc = input_update(&f->in, error, sizeof(error));
    if (!rc) return false;
    if (rc < 0) { say("%s", error); snprintf(stream_failure, sizeof(stream_failure), "%s", error); }
    data = f->in.data;
    size = f->in.size;
    /* Only a decompressed stream replaces what was shown, and that was empty. */
    if (data != old && old_size) { lines_reset(); top = settle(0); }
    if (opt.header_lines) compute_header();
    return true;
}
/* Beyond its read-ahead, a stream is read as far as the screen needs. */
static void demand_screen(void) {
    if (live()) input_demand(&files[current].in, page_end == SIZE_MAX ? size + 1 : 0);
}
/* Waits for more of the current document while it is being read, asking
   for `want` bytes and showing `note`: 1 when more arrived or it ended, 0 if
   it was complete, -1 when Ctrl-C or a key (left for the next command)
   stopped the wait. */
static void show_note(const char *note) {
    char saved[sizeof(message)];
    memcpy(saved, message, sizeof(message));
    snprintf(message, sizeof(message), "%s", note);
    draw();
    memcpy(message, saved, sizeof(message));
}
static int wait_for_input(size_t want, const char *note) {
    static struct timespec shown;
    if (!live()) return 0;
    input_demand(&files[current].in, want);
    for (;;) {
        if (stream_refresh()) {
            /* While input keeps coming, nothing blocks: the screen is still
               shown a few times a second, and keys are still seen. */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((double)(now.tv_sec - shown.tv_sec) + (double)(now.tv_nsec - shown.tv_nsec) / 1e9 >= 0.25) {
                shown = now;
                show_note(note);
                if (keys_waiting()) {
                    int c = key_read(0);
                    if (c >= 0 && c != opt.intr_char && c != 0x03) { char k = (char)c; keys_queue(&k, 1, true); }
                    return -1;
                }
            }
            return 1;
        }
        show_note(note);
        clock_gettime(CLOCK_MONOTONIC, &shown);
        int c = key_wait(-1);
        if (c == KEY_WAKE) { input_wake_drain(); continue; }
        if (c == KEY_SIGNAL) {
            if (interrupted || exiting || suspended) return -1;
            resized = 0;
            continue;
        }
        if (c >= 0 && c != opt.intr_char && c != 0x03) { char k = (char)c; keys_queue(&k, 1, true); }
        return -1;
    }
}
static const char *wait_note(void) {
    return opt.prompt[PROMPT_WAIT] ? opt.prompt[PROMPT_WAIT] : default_prompts[PROMPT_WAIT];
}
/* Reads the current document to its end, as for G and %: false if stopped. */
static bool read_to_end(void) {
    int rc;
    while ((rc = wait_for_input(SIZE_MAX, "Reading to the end of the input... (interrupt to stop)")) == 1) {}
    return rc == 0;
}
/* Waits up to 100 ms for news from streams, without the pager. */
static void await_news(void) {
    struct pollfd p = {.fd = input_wake_fd(), .events = POLLIN};
    if (p.fd >= 0) poll(&p, 1, 100);
    input_wake_drain();
}

/* ---- Movement ---- */
static void quit(int code) { quitting = true; exit_code = code; }
static void forward(int n, bool force, bool page) {
    layout_rows();
    bool was_eof = page_end == SIZE_MAX;
    size_t first_new = page_end;
    int moved = 0;
    while (moved < n) {
        size_t q;
        /* At the end of a stream being read, as in less, wait for more. */
        if ((!force && page_end == SIZE_MAX) || fwd(top, &q) != 1) {
            if (wait_for_input(size + 1, wait_note()) != 1) break;
            layout_rows();
            continue;
        }
        top = q;
        ++moved;
        if (page_end != SIZE_MAX) {
            size_t r;
            page_end = fwd(page_end, &r) == 1 ? r : SIZE_MAX;
        }
    }
    unread_line = moved && (opt.unread == 2 || (opt.unread == 1 && page)) ? first_new : SIZE_MAX;
    if (!moved) bell(true);
    if (opt.quit_eof == 1 && was_eof && !moved && !live()) quit(0);
}
static void backward(int n) {
    int moved = 0;
    for (; moved < n; ++moved) {
        size_t q;
        if (back(top, &q) != 1) break;
        top = q;
    }
    if (!moved) bell(true);
}
static void jump_to(size_t pos, int row) {
    top = back_lines(settle(pos), row - 1);
}
static void show_end(void) {
    top = size ? back_lines(settle(size), body_rows() - 1) : 0;
}
/* While a stream is being read, the end is shown as it moves until it is
   reached or the wait is stopped. */
static void goto_end(void) {
    for (;;) {
        show_end();
        if (wait_for_input(SIZE_MAX, "Reading to the end of the input... (interrupt to stop)") != 1) break;
    }
}
/* Shifts horizontally so that the match at hit is visible. */
static void show_column(size_t hit) {
    size_t begin = line_start(hit);
    int col = column_of(begin, line_end(begin), hit);
    int left = opt.header_cols, width = text_width() - opt.header_cols;
    if (col < left) return;
    if (col < left + shift || col >= left + shift + width) {
        int s = col - left - opt.match_shift;
        shift = s < 0 ? 0 : s;
    }
}

/* ---- Searching ---- */
static Flags default_flags(void) {
    Flags f = {0};
    for (const char *p = opt.search_options; *p; ++p) {
        if (*p == 'E') f.multi = true;
        else if (*p == 'F') f.first = true;
        else if (*p == 'K') f.keep = true;
        else if (*p == 'N') f.invert = true;
        else if (*p == 'R') f.no_regex = true;
        else if (*p == 'W') f.wrap = true;
    }
    return f;
}
static Flags prompt_flags;
static char prompt_kind;
static bool awaiting_sub;
static void flags_label(char *label, size_t cap) {
    snprintf(label, cap, "%s%s%s%s%s%s", prompt_flags.invert ? "Non-match " : "",
             prompt_flags.multi ? "EOF-ignore " : "", prompt_flags.first ? "First-file " : "",
             prompt_flags.keep ? "Keep-pos " : "", prompt_flags.no_regex ? "Regex-off " : "",
             prompt_flags.wrap ? "Wrap " : "");
    size_t n = strlen(label);
    if (prompt_flags.sub) snprintf(label + n, cap - n, "Subpattern %d ", prompt_flags.sub), n = strlen(label);
    snprintf(label + n, cap - n, "%c", prompt_kind);
}
static bool search_modifier(int c, char *label, size_t cap) {
    if (awaiting_sub) {
        awaiting_sub = false;
        if (c >= '1' && c <= '9') { prompt_flags.sub = c - '0'; flags_label(label, cap); return true; }
        return false;
    }
    bool filter_prompt = prompt_kind == '&';
    /* The punctuation forms are less's; literal and glob patterns often
       begin with them, so there only the control keys are modifiers. */
    if ((c == '!' || c == '*' || c == '@') && (opt.mode != REGEX || prompt_flags.no_regex)) return false;
    switch (c) {
    case 0x0e: case '!': prompt_flags.invert = !prompt_flags.invert; break;
    case 0x12: prompt_flags.no_regex = !prompt_flags.no_regex; break;
    case 0x05: case '*': if (filter_prompt) return false; prompt_flags.multi = !prompt_flags.multi; break;
    case 0x06: case '@': if (filter_prompt) return false; prompt_flags.first = !prompt_flags.first; break;
    case 0x0b: if (filter_prompt) return false; prompt_flags.keep = !prompt_flags.keep; break;
    case 0x17: if (filter_prompt) return false; prompt_flags.wrap = !prompt_flags.wrap; break;
    case 0x13: if (filter_prompt) return false; awaiting_sub = true; return true;
    default: return false;
    }
    flags_label(label, cap);
    return true;
}
/* A hit is usable unless the filter hides its line or it lies in a header
   excluded from searching. */
static bool acceptable(size_t hit, bool invert) {
    size_t line = line_start(hit);
    if (opt.no_search_header_lines && header_rows && line >= header_begin && line < header_end) return false;
    if (filter_on && shown(line) != 1) return false;
    if (!invert && opt.no_search_header_cols && opt.header_cols &&
        column_of(line, line_end(line), hit) < opt.header_cols)
        return false;
    return true;
}
/* One search step from start (forward) or before start (backward), within
   [floor, limit). */
static int find_from(const Search *s, bool invert, int dir, size_t start, size_t floor, size_t limit,
                     size_t *hit, char *error, size_t cap) {
    for (;;) {
        int rc;
        if (dir > 0) {
            if (start >= limit) return 0;
            rc = invert ? nonmatch_forward(s, line_start(start) == start ? start : next_line(start), hit, error, cap)
                        : search_forward(s, start, limit, hit, error, cap);
            if (rc == 1 && *hit >= limit) return 0;
        } else {
            if (start <= floor) return 0;
            rc = invert ? nonmatch_backward(s, start, hit, error, cap) : search_backward(s, start, hit, error, cap);
            if (rc == 1 && *hit < floor) return 0;
        }
        if (rc != 1) return rc;
        if (acceptable(*hit, invert)) return 1;
        if (dir > 0) start = invert ? next_line(*hit) : *hit + 1;
        else start = *hit;
    }
}
static size_t after_hit(const Search *s, bool invert, size_t hit) {
    return invert || s->mode == GLOB ? next_line(hit) : hit + 1;
}
static size_t target_line(void) {
    layout_rows();
    size_t p = row_pos[target_row() - 1];
    return p == SIZE_MAX ? top : p;
}
static void run_search(int dir, bool repeat, int count, Flags f) {
    if (!search_text) { say("No previous search pattern"); return; }
    char error[256] = "";
    size_t start;
    int origin_file = current;
    size_t origin_top = top;
    if (f.first && f.multi && !viewing_help && file_count > 1) {
        if (!switch_file(dir > 0 ? 0 : file_count - 1)) return;
        start = dir > 0 ? body_start() : size;
    } else if (f.first) start = dir > 0 ? body_start() : size;
    else if (repeat && have_hit) start = dir > 0 ? after_hit(&search, f.invert, last_hit) : last_hit;
    else if (dir > 0) {
        size_t t = target_line();
        start = opt.skip_screen == 1 ? (page_end == SIZE_MAX ? size : page_end) :
                opt.skip_screen == 2 ? next_line(t) : t;
    } else start = opt.skip_screen == 1 ? top : target_line();
    printf("\033[%d;1H\033[0m\033[2KSearching... (Ctrl-C cancels)", rows);
    fflush(stdout);
    size_t hit = 0;
    bool wrapped = false;
    for (int i = 0; i < count; ++i) {
        size_t origin = start;
        int rc = find_from(&search, f.invert, dir, start, body_start(), live() ? line_start(size) : size,
                           &hit, error, sizeof(error));
        /* Forward, a stream being read is searched as it arrives; its last,
           unfinished line waits until it is complete. */
        while (!rc && dir > 0 && live()) {
            size_t searched = line_start(size);
            if (wait_for_input(SIZE_MAX, "Searching the input as it arrives... (interrupt to stop)") != 1) {
                rc = -1;
                if (!interrupted) snprintf(error, sizeof(error), "Search stopped");
                break;
            }
            rc = find_from(&search, f.invert, dir, searched > start ? searched : start, body_start(),
                           live() ? line_start(size) : size, &hit, error, sizeof(error));
        }
        if (!rc && f.wrap) {
            rc = dir > 0 ? find_from(&search, f.invert, 1, body_start(), body_start(), origin, &hit, error, sizeof(error))
                         : find_from(&search, f.invert, -1, size, origin, size, &hit, error, sizeof(error));
            if (rc == 1) wrapped = true;
        }
        while (!rc && f.multi && !viewing_help) {
            int next = current + dir;
            if (next < 0 || next >= file_count || !switch_file(next)) break;
            rc = find_from(&search, f.invert, dir, dir > 0 ? body_start() : size, body_start(), size,
                           &hit, error, sizeof(error));
        }
        if (rc != 1) {
            if (current != origin_file) { switch_file(origin_file); top = origin_top; }
            if (rc < 0) say("%s", interrupted ? "Search cancelled" : error);
            else if (stream_failure[0]) say("Pattern not found; %s", stream_failure);
            else say("Pattern not found");
            stream_failure[0] = 0;
            return;
        }
        start = dir > 0 ? after_hit(&search, f.invert, hit) : hit;
    }
    if (current == origin_file) remember_position();
    last_hit = hit;
    last_hit_end = f.invert ? hit : match_end(&search, hit);
    have_hit = true;
    hilite_off = false;
    jump_to(hit, target_row());
    if (!f.invert) show_column(hit);
    say("%sMatch at byte %zu", wrapped ? "(wrapped) " : "", hit);
}
static bool compile_search(Search *s, const char *text, Flags f, char *error, size_t cap) {
    Mode mode = f.no_regex ? LITERAL : (Mode)opt.mode;
    return set_search(s, mode, text, opt.case_mode, f.sub, error, cap);
}
/* --incsearch: a match for the pattern typed so far is shown as it changes. */
static size_t inc_origin;
static int inc_dir;
static Search inc_search;
static volatile sig_atomic_t watch_stop, key_cancel;
static void *watch_keys(void *arg) {
    (void)arg;
    struct pollfd p = {.fd = tty_fd, .events = POLLIN};
    while (!watch_stop) {
        if (poll(&p, 1, 20) > 0) { key_cancel = 1; interrupted = 1; break; }
    }
    return NULL;
}
static void incremental(const char *text) {
    if (!opt.incsearch || keys_queued()) return;
    top = inc_origin;
    preview = NULL;
    char error[256];
    if (*text && compile_search(&inc_search, text, prompt_flags, error, sizeof(error))) {
        watch_stop = 0; key_cancel = 0;
        pthread_t watcher;
        bool watching = !pthread_create(&watcher, NULL, watch_keys, NULL);
        size_t hit;
        int rc = find_from(&inc_search, prompt_flags.invert, inc_dir, inc_origin, body_start(), size, &hit,
                           error, sizeof(error));
        watch_stop = 1;
        if (watching) pthread_join(watcher, NULL);
        if (key_cancel) { interrupted = 0; return; }
        if (rc == 1) {
            jump_to(hit, target_row());
            if (!prompt_flags.invert) preview = &inc_search;
        }
    }
    draw();
}
static void search_command(int dir, int count) {
    char label[128];
    prompt_flags = default_flags();
    prompt_kind = dir > 0 ? '/' : '?';
    awaiting_sub = false;
    flags_label(label, sizeof(label));
    PromptSpec spec = {.label = label, .label_cap = sizeof(label), .history = H_SEARCH,
                       .modifier = search_modifier, .changed = incremental};
    char text[4096] = "";
    inc_origin = top;
    inc_dir = dir;
    bool ok = prompt_line(&spec, text, sizeof(text));
    top = inc_origin;
    preview = NULL;
    if (!ok) return;
    Flags f = prompt_flags;
    if (*text) {
        char error[256];
        Search s = {0};
        if (!compile_search(&s, text, f, error, sizeof(error))) { say("%s", error); return; }
        clear_search(&search);
        search = s;
        free(search_text);
        search_text = strdup(text);
    } else if (!search_text) { say("No previous search pattern"); return; }
    else {
        char error[256];
        Search s = {0};
        if (!compile_search(&s, search_text, f, error, sizeof(error))) { say("%s", error); return; }
        clear_search(&search);
        search = s;
    }
    search_flags = f;
    search_dir = dir;
    have_hit = false;
    if (f.keep) { hilite_off = false; return; }
    run_search(dir, false, count, f);
}
static void filter_command(void) {
    char label[128];
    prompt_flags = (Flags){0};
    prompt_kind = '&';
    flags_label(label, sizeof(label));
    PromptSpec spec = {.label = label, .label_cap = sizeof(label), .history = H_SEARCH,
                       .modifier = search_modifier};
    char text[4096] = "";
    if (!prompt_line(&spec, text, sizeof(text))) return;
    if (!*text) {
        filter_on = false;
        clear_search(&filter);
        top = settle(top);
        say("Showing all lines");
        return;
    }
    char error[256];
    Search s = {0};
    if (!compile_search(&s, text, prompt_flags, error, sizeof(error))) { say("%s", error); return; }
    clear_search(&filter);
    filter = s;
    filter_on = true;
    filter_invert = prompt_flags.invert;
    top = settle(top);
    say("Showing only lines %smatching the pattern", filter_invert ? "not " : "");
}

/* ---- Jumps ---- */
static void goto_line(size_t n) {
    printf("\033[%d;1H\033[0m\033[2KCalculating line numbers... (Ctrl-C cancels)", rows);
    fflush(stdout);
    size_t pos;
    int rc;
    while (!(rc = line_position(n, &pos)) && wait_for_input(SIZE_MAX, "Reading input to find the line... (interrupt to stop)") == 1) {}
    if (rc < 0) { say("Line counting interrupted"); return; }
    remember_position();
    if (rc) jump_to(pos, target_row());
    else { show_end(); say("Line %zu is past the end of the file", n); }
}
static void goto_mark(int c) {
    if (c == '^') { remember_position(); top = body_start(); top = settle(top); return; }
    if (c == '$') { remember_position(); goto_end(); return; }
    if (c == 0x18) c = '\'';
    Mark m = marks[c & 0xff];
    if (!m.set || !m.file) { if (c == '\'') { top = body_start(); top = settle(top); } else say("Mark not set"); return; }
    if (c == '\'' || c == '#' || isalpha(c)) {
        Mark here = {'\'', mark_file() ? strdup(mark_file()) : NULL, top, 1, true};
        char *file = strdup(m.file);
        if (viewing_help) close_help();
        if (file && strcmp(file, mark_file() ? mark_file() : "")) {
            int i = find_file(file);
            if (i < 0 || !switch_file(i)) { say("File for mark is not in the file list"); free(file); free(here.file); return; }
        }
        free(file);
        jump_to(m.pos, m.row > 0 ? m.row : 1);
        free(marks['\''].file);
        marks['\''] = here;
        return;
    }
    say("Invalid mark");
}
static void match_bracket(bool forward_dir, int open, int close, int n) {
    if (n < 1) n = 1;
    layout_rows();
    size_t line = forward_dir ? top : bottom_line(), end = line_end(line), pos = SIZE_MAX;
    if (forward_dir) {
        for (size_t q = line; q < end; ++q)
            if (data[q] == open && !--n) { pos = q; break; }
    } else {
        for (size_t q = end; q > line; --q)
            if (data[q - 1] == close && !--n) { pos = q - 1; break; }
    }
    if (pos == SIZE_MAX) { say("No bracket in %s line", forward_dir ? "top" : "bottom"); return; }
    size_t depth = 1, found = SIZE_MAX;
    if (forward_dir) {
        for (size_t q = pos + 1; q < size; ++q) {
            if (!(q & 65535) && interrupted) break;
            if (data[q] == open && open != close) ++depth;
            else if (data[q] == close && !--depth) { found = q; break; }
        }
    } else {
        for (size_t q = pos; q > 0; --q) {
            if (!(q & 65535) && interrupted) break;
            if (data[q - 1] == close && open != close) ++depth;
            else if (data[q - 1] == open && !--depth) { found = q - 1; break; }
        }
    }
    if (found == SIZE_MAX) { say(interrupted ? "Interrupted" : "No matching bracket"); return; }
    remember_position();
    if (forward_dir) jump_to(found, body_rows());
    else jump_to(found, 1);
}

/* ---- Follow mode ---- */
static void follow(bool until_hilite) {
    if (viewing_help) return;
    File *f = &files[current];
    show_end();
    for (;;) {
        char saved[sizeof(message)];
        memcpy(saved, message, sizeof(message));
        snprintf(message, sizeof(message), "%s", opt.prompt[PROMPT_WAIT] ? opt.prompt[PROMPT_WAIT] : default_prompts[PROMPT_WAIT]);
        draw();
        memcpy(message, saved, sizeof(message));
        /* A stream being read is read on as far as it goes. */
        if (f->in.reading) input_demand(&f->in, SIZE_MAX);
        int c = key_wait(200);
        if (c == KEY_WAKE) input_wake_drain();
        if (c == KEY_SIGNAL) {
            if (interrupted || exiting || suspended) { interrupted = 0; break; }
            resized = 0;
            continue;
        }
        if (c >= 0) {
            /* Any key ends following; it is then read as a command. */
            if (c != opt.intr_char && c != 0x03) { char k = (char)c; keys_queue(&k, 1, true); }
            break;
        }
        if (c == KEY_EOF) break;
        size_t old = size;
        char error[256];
        int rc = 0;
        if (f->in.stream) {
            if (!stream_refresh()) continue;
            rc = 1;
        } else if (opt.follow_name && !f->is_stdin) {
            struct stat st;
            if (!stat(f->name, &st) && f->in.mapped &&
                ((unsigned long long)st.st_ino != f->in.ino || (unsigned long long)st.st_dev != f->in.dev)) {
                reload_current();
                old = 0;
                rc = 1;
            }
        }
        if (!rc) rc = input_refresh(&f->in, error, sizeof(error));
        if (rc < 0) { say("%s", error); break; }
        if (!rc) continue;
        data = f->in.data;
        size = f->in.size;
        if (size < old) { lines_reset(); old = 0; }
        if (message[0] && f->in.stream && !f->in.reading) break; /* A read error. */
        if (until_hilite && search_text && !search_flags.invert) {
            size_t hit;
            if (search_forward(&search, old ? line_start(old - 1) : 0, size, &hit, error, sizeof(error)) == 1) {
                last_hit = hit; last_hit_end = match_end(&search, hit); have_hit = true;
                jump_to(hit, target_row());
                say("Match at byte %zu", hit);
                return;
            }
        }
        show_end();
    }
    if (!f->in.mapped && !f->in.stream && !f->is_stdin) say("Only regular files grow; showing the end");
}

/* ---- Shell commands ---- */
static char last_shell[8192];
/* Expands % to the current file, # to the previous one, and !! to the
   previous command, as less does. */
static void expand_command(const char *in, char *out, size_t cap, bool bang) {
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap; ++p) {
        const char *insert = NULL;
        char one[2] = {*p, 0};
        if (*p == '\\' && (p[1] == '%' || p[1] == '#' || p[1] == '!')) { one[0] = *++p; insert = one; }
        else if (*p == '%' && current >= 0 && !viewing_help) insert = files[current].name;
        else if (*p == '#' && previous_file >= 0) insert = files[previous_file].name;
        else if (bang && *p == '!') insert = last_shell;
        else insert = one;
        size_t m = strlen(insert);
        if (n + m >= cap) m = cap - n - 1;
        memcpy(out + n, insert, m);
        n += m;
    }
    out[n] = 0;
}
static void wait_return(void) {
    fputs("\n!done  (press RETURN)", stdout);
    fflush(stdout);
    for (;;) {
        int c = key_read(-1);
        if (c == '\r' || c == '\n' || c < 0 || c == ' ' || c == 'q') break;
    }
}
static void run_shell(const char *cmd, bool pause) {
    leave_screen();
    pid_t pid = fork();
    if (!pid) {
        child_defaults();
        if (tty_fd != STDIN_FILENO) dup2(tty_fd, STDIN_FILENO);
        const char *shell = shell_path();
        if (*cmd) execl(shell, shell, "-c", cmd, (char *)NULL);
        else execl(shell, shell, (char *)NULL);
        _exit(127);
    }
    int status;
    if (pid > 0) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    interrupted = 0;
    enter_screen();
    if (pause) {
        /* The output stays on the normal screen until RETURN. */
        if (!opt.no_init) fputs("\033[?1049l", stdout);
        wait_return();
        if (!opt.no_init) fputs("\033[?1049h", stdout);
    }
}
static void pipe_range(size_t from, size_t to, const char *cmd) {
    leave_screen();
    int fds[2];
    if (pipe(fds)) { enter_screen(); say("pipe: %s", strerror(errno)); return; }
    pid_t pid = fork();
    if (!pid) {
        child_defaults();
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]); close(fds[1]);
        const char *shell = shell_path();
        execl(shell, shell, "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    void (*old)(int) = signal(SIGPIPE, SIG_IGN);
    for (size_t p = from; p < to && pid > 0 && !interrupted;) {
        size_t n = to - p > (1u << 20) ? (1u << 20) : to - p;
        ssize_t w = write(fds[1], data + p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        p += (size_t)w;
    }
    close(fds[1]);
    signal(SIGPIPE, old);
    int status;
    if (pid > 0) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    interrupted = 0;
    enter_screen();
    if (!opt.no_init) fputs("\033[?1049l", stdout);
    wait_return();
    if (!opt.no_init) fputs("\033[?1049h", stdout);
}
static void pipe_command(void) {
    int m = prompt_key("|mark: ");
    if (m < 0) return;
    char label[16], cmd[4096] = "";
    snprintf(label, sizeof(label), "|%c!", m == '\r' || m == '\n' ? '.' : m);
    if (!simple_prompt(label, cmd, sizeof(cmd), H_SHELL)) return;
    layout_rows();
    size_t screen_end = page_end == SIZE_MAX ? size : page_end, from = top, to = screen_end;
    if (m == '^') from = 0;
    else if (m == '$') to = size;
    else if (m != '.' && m != '\r' && m != '\n') {
        const Mark *mk = &marks[m & 0xff];
        if (!mk->set || !mk->file || !mark_file() || strcmp(mk->file, mark_file())) { say("Mark not set"); return; }
        if (mk->pos < from) from = line_start(mk->pos);
        else to = next_line(mk->pos) > to ? next_line(mk->pos) : to;
    }
    char expanded[4096];
    expand_command(cmd, expanded, sizeof(expanded), false);
    pipe_range(from, to, expanded);
}
static void save_command(void) {
    char name[4096] = "";
    if (!simple_prompt("log file: ", name, sizeof(name), H_NONE) || !*name) return;
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0 && errno == EEXIST) {
        int c = prompt_key("Warning: \"file\" exists; Overwrite, Append, or Don't log? ");
        if (c == 'O' || c == 'o') fd = open(name, O_WRONLY | O_TRUNC);
        else if (c == 'A' || c == 'a') fd = open(name, O_WRONLY | O_APPEND);
        else return;
    }
    if (fd < 0) { say("%s: %s", name, strerror(errno)); return; }
    bool ok = true;
    for (size_t p = 0; p < size && ok && !interrupted;) {
        size_t n = size - p > (1u << 20) ? (1u << 20) : size - p;
        ssize_t w = write(fd, data + p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = false;
        else p += (size_t)w;
    }
    if (close(fd)) ok = false;
    if (interrupted) say("Save interrupted");
    else if (ok) say("Saved %s", name);
    else say("Write to %s failed", name);
}
static void edit_command(void) {
    if (viewing_help || files[current].is_stdin) { say("Cannot edit standard input"); return; }
    char cmd[8192];
    const char *lessedit = getenv("LESSEDIT");
    layout_rows();
    count_lines_now(position_at('m'));
    expand_prompt(lessedit && *lessedit ? lessedit : "%E ?lm+%lm. %g", cmd, sizeof(cmd));
    run_shell(cmd, false);
    reload_current();
}

/* ---- Options at runtime ---- */
static void apply_settings(void) {
    apply_render();
    if (search_text) {
        char error[256];
        Search s = {0};
        if (compile_search(&s, search_text, search_flags, error, sizeof(error))) { clear_search(&search); search = s; }
    }
    if (raw_mode) set_mouse(opt.mouse);
    compute_header();
    top = settle(top);
}
static void option_command(bool display_only) {
    int c = prompt_key(display_only ? "_" : "-");
    if (c < 0) return;
    int how = HOW_TOGGLE;
    char lead[4] = "-";
    if (!display_only && (c == '+' || c == '!')) {
        how = c == '+' ? HOW_RESET : HOW_OPPOSITE;
        lead[1] = (char)c;
        c = prompt_key(lead);
        if (c < 0) return;
    }
    const OptionDef *o;
    char error[256] = "";
    if (c == (display_only ? '_' : '-')) {
        char name[256] = "";
        if (!simple_prompt(display_only ? "__" : "--", name, sizeof(name), H_NONE) || !*name) return;
        o = option_long(name, strcspn(name, "="), error, sizeof(error));
        if (!o) { say("%s", error); return; }
        if (!display_only && strchr(name, '=') && option_runtime(o)) {
            option_apply(o, strchr(name, '=') + 1, HOW_SET, message, sizeof(message));
            apply_settings();
            return;
        }
    } else {
        o = option_short(c);
        if (!o) { say("There is no -%c option", isprint(c) ? c : '?'); return; }
    }
    if (display_only) { option_describe(o, message, sizeof(message)); return; }
    if (!option_supported(o)) { option_apply(o, NULL, HOW_TOGGLE, message, sizeof(message)); return; }
    if (!option_runtime(o)) { say("The --%s option cannot be changed now", option_name(o)); return; }
    char value[1024] = "";
    const char *v = NULL;
    if (option_takes_value(o) && how != HOW_RESET) {
        char label[300];
        snprintf(label, sizeof(label), "--%s=", option_name(o));
        if (!simple_prompt(label, value, sizeof(value), H_NONE)) return;
        if (!*value) { option_describe(o, message, sizeof(message)); return; }
        v = value;
        how = HOW_SET;
    }
    option_apply(o, v, how, message, sizeof(message));
    apply_settings();
}

/* ---- Mouse ---- */
static void mouse_event(int button, int row, bool release) {
    int wheel = opt.wheel_lines > 0 ? opt.wheel_lines : 1;
    bool reverse = opt.mouse == 2;
    if (button == 64) { if (reverse) forward(wheel, false, false); else backward(wheel); }
    else if (button == 65) { if (reverse) backward(wheel); else forward(wheel, false, false); }
    else if ((button & 3) == 0 && release) {
        int r = row - 1 - header_rows;
        layout_rows();
        if (r >= 0 && r < body_rows() && row_pos[r] != SIZE_MAX) set_mark('#', row_pos[r], r + 1);
    }
}
static void read_mouse(bool sgr) {
    if (!sgr) {
        int b = key_read(100), x = key_read(100), y = key_read(100);
        (void)x;
        if (b < 0 || y < 0) return;
        b = (b - 32) & ~28;
        /* X10 reports a release without saying which button. */
        if (b >= 64) mouse_event(b, y - 32, false);
        else if ((b & 3) == 3) mouse_event(0, y - 32, true);
        return;
    }
    int values[3] = {0}, i = 0, c;
    while ((c = key_read(100)) >= 0 && c != 'M' && c != 'm') {
        if (c == ';') { if (i < 2) ++i; }
        else if (c >= '0' && c <= '9' && values[i] < 100000) values[i] = values[i] * 10 + c - '0';
    }
    if (c < 0) return;
    mouse_event(values[0] & ~28, values[2], c == 'm');
}

/* ---- Main loop ---- */
static void suspend(void) {
    leave_screen();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
    /* Resumed. */
    struct sigaction sa;
    sigaction(SIGINT, NULL, &sa);
    sigaction(SIGTSTP, &sa, NULL);
    suspended = 0;
    enter_screen();
}
/* Reads a count typed before a command: digits and a decimal point. */
static bool woke; /* news from a stream ended the wait for a command */
static bool read_number(double *value) {
    char digits[32];
    size_t n = 0;
    int c = key_wait(-1);
    if (c == KEY_WAKE) { woke = true; return false; }
    if (c < 0) { if (c == KEY_SIGNAL) return false; return false; }
    if (!(c >= '0' && c <= '9') && c != '.') { char k = (char)c; keys_queue(&k, 1, true); return false; }
    digits[n++] = (char)c;
    for (;;) {
        printf("\033[%d;1H\033[0m\033[2K:%.*s", rows, (int)n, digits);
        fflush(stdout);
        c = key_read(-1);
        if (c == KEY_SIGNAL) return false;
        if (c < 0) break;
        if ((c >= '0' && c <= '9') || (c == '.' && !memchr(digits, '.', n))) { if (n < sizeof(digits) - 1) digits[n++] = (char)c; continue; }
        if (c == 0x7f || c == '\b') { if (!--n) return false; continue; }
        char k = (char)c;
        keys_queue(&k, 1, true);
        break;
    }
    digits[n] = 0;
    *value = strtod(digits, NULL);
    return true;
}
static void examine_command(void) {
    if (secure) { say("Command not available"); return; }
    char name[4096] = "";
    if (!simple_prompt("Examine: ", name, sizeof(name), H_NONE)) return;
    char expanded[4096];
    expand_command(name, expanded, sizeof(expanded), false);
    if (!*expanded) {
        if (viewing_help) close_help();
        reload_current();
        return;
    }
    if (viewing_help) close_help();
    /* Names are separated by spaces; each is added after the current one. */
    int at = current + 1, first = -1;
    for (char *save = NULL, *tok = strtok_r(expanded, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        int existing = find_file(tok);
        if (existing >= 0) { if (first < 0) first = existing; continue; }
        if (!add_file(at, tok)) break;
        if (first < 0) first = at;
        ++at;
    }
    if (first >= 0) switch_file(first);
}
static void command(Action a, const char *extra, bool has_number, double number) {
    long n = has_number ? (long)number : 0;
    if (n < 0) n = 0;
    int count = has_number && n > 0 ? (int)(n > INT_MAX ? INT_MAX : n) : 1;
    switch (a) {
    case A_FORW_LINE: forward(count, false, false); break;
    case A_FORW_LINE_FORCE: forward(count, true, false); break;
    case A_BACK_LINE: case A_BACK_LINE_FORCE: backward(count); break;
    case A_FORW_SCREEN: forward(has_number ? count : window_lines(), false, true); break;
    case A_FORW_SCREEN_FORCE: forward(has_number ? count : window_lines(), true, true); break;
    case A_BACK_SCREEN: backward(has_number ? count : window_lines()); break;
    case A_FORW_WINDOW: if (has_number) opt.window = count; forward(window_lines(), false, true); break;
    case A_BACK_WINDOW: if (has_number) opt.window = count; backward(window_lines()); break;
    case A_FORW_SCROLL: if (has_number) half_window = count; forward(half_window ? half_window : body_rows() / 2 ? body_rows() / 2 : 1, false, false); break;
    case A_BACK_SCROLL: if (has_number) half_window = count; backward(half_window ? half_window : body_rows() / 2 ? body_rows() / 2 : 1); break;
    case A_RIGHT_SCROLL:
        if (has_number) { opt.shift = count; opt.shift_is_fraction = 0; }
        shift = shift > INT_MAX / 2 ? shift : shift + shift_step();
        break;
    case A_LEFT_SCROLL:
        if (has_number) { opt.shift = count; opt.shift_is_fraction = 0; }
        shift = shift > shift_step() ? shift - shift_step() : 0;
        break;
    case A_RIGHT_END: {
        layout_rows();
        int widest = 0;
        for (int r = 0; r < body_rows(); ++r) {
            if (row_pos[r] == SIZE_MAX) break;
            int w = line_width(row_pos[r], line_end(row_pos[r]), (size_t)64 << 20);
            if (w > widest) widest = w;
        }
        int room = text_width() - opt.header_cols;
        shift = widest - opt.header_cols > room ? widest - opt.header_cols - room : 0;
        break;
    }
    case A_LEFT_END: shift = 0; break;
    case A_FORW_FOREVER: follow(false); break;
    case A_FORW_UNTIL_HILITE: follow(true); break;
    case A_REPAINT: break;
    case A_FLUSH_REPAINT:
        tcflush(tty_fd, TCIFLUSH);
        keys_discard();
        if (!viewing_help && current >= 0 && files[current].in.mapped) {
            char error[256];
            size_t old = size;
            if (input_refresh(&files[current].in, error, sizeof(error)) > 0) {
                data = files[current].in.data;
                size = files[current].in.size;
                if (size < old) lines_reset();
                top = settle(top);
            }
        }
        stream_refresh();
        break;
    case A_F_SEARCH: search_command(1, count); break;
    case A_B_SEARCH: search_command(-1, count); break;
    case A_AGAIN_SEARCH: case A_REVERSE_SEARCH: case A_T_AGAIN_SEARCH: case A_T_REVERSE_SEARCH: {
        Flags f = search_flags;
        f.first = f.keep = false;
        if (a == A_T_AGAIN_SEARCH || a == A_T_REVERSE_SEARCH) f.multi = true;
        int dir = a == A_REVERSE_SEARCH || a == A_T_REVERSE_SEARCH ? -search_dir : search_dir;
        if (!search_text) { say("No search pattern"); break; }
        if (have_hit && dir > 0 && last_hit >= size) { say("No further match"); break; }
        run_search(dir, true, count, f);
        break;
    }
    case A_UNDO_HILITE: hilite_off = !hilite_off; break;
    case A_CLEAR_SEARCH:
        hilite_off = false;
        free(search_text);
        search_text = NULL;
        clear_search(&search);
        have_hit = false;
        break;
    case A_FILTER: filter_command(); break;
    case A_GOLINE:
        if (has_number && n > 1) goto_line((size_t)n);
        else { remember_position(); top = settle(body_start()); }
        break;
    case A_GOEND:
        remember_position();
        if (has_number) goto_line(n ? (size_t)n : 1);
        else goto_end();
        break;
    case A_PERCENT: {
        double pct = has_number ? number : 0;
        if (pct > 100) pct = 100;
        remember_position();
        if (pct > 0 && !read_to_end()) say("Percentages are of the input read so far");
        size_t pos = (size_t)((double)size * pct / 100.0);
        if (pos >= size && size) pos = size - 1;
        jump_to(pos, target_row());
        break;
    }
    case A_GOPOS:
        remember_position();
        while (has_number && number >= (double)size &&
               wait_for_input((size_t)number + 1, "Reading input to reach the byte... (interrupt to stop)") == 1) {}
        jump_to(has_number ? (size_t)number : 0, target_row());
        break;
    case A_F_BRACKET: case A_B_BRACKET: {
        int open, close;
        if (extra && strlen(extra) >= 2) { open = (unsigned char)extra[0]; close = (unsigned char)extra[1]; }
        else {
            open = prompt_key("Brackets: ");
            close = open < 0 ? -1 : key_read(-1);
            if (open < 0 || close < 0) break;
        }
        match_bracket(a == A_F_BRACKET, open, close, count);
        break;
    }
    case A_SETMARK: case A_SETMARKBOT: {
        int c = prompt_key("mark: ");
        if (c < 0) break;
        if (!isalpha(c)) { say("Choose a letter between 'a' and 'Z'"); break; }
        layout_rows();
        if (a == A_SETMARK) set_mark(c, top, 1);
        else set_mark(c, bottom_line(), body_rows());
        break;
    }
    case A_GOMARK: {
        int c = prompt_key("goto mark: ");
        if (c >= 0) goto_mark(c);
        break;
    }
    case A_CLRMARK: {
        int c = prompt_key("clear mark: ");
        if (c < 0) break;
        Mark *m = &marks[c & 0xff];
        if (!m->set) say("Mark not set");
        free(m->file);
        *m = (Mark){0};
        break;
    }
    case A_EXAMINE: examine_command(); break;
    case A_NEXT_FILE:
        if (viewing_help) close_help();
        if (current + count >= file_count) { say("No next file"); bell(false); }
        else switch_file(current + count);
        break;
    case A_PREV_FILE:
        if (viewing_help) close_help();
        if (current - count < 0) { say("No previous file"); bell(false); }
        else switch_file(current - count);
        break;
    case A_INDEX_FILE:
        if (viewing_help) close_help();
        if (count > file_count) say("No such file");
        else switch_file(count - 1);
        break;
    case A_REMOVE_FILE:
        if (viewing_help) close_help();
        if (file_count < 2) { say("No other file"); break; }
        {
            int gone = current, next = current + 1 < file_count ? current + 1 : current - 1;
            if (!switch_file(next)) break;
            close_document(&files[gone]);
            lines_free(files[gone].lines);
            free(files[gone].name);
            memmove(files + gone, files + gone + 1, (size_t)(file_count - gone - 1) * sizeof(File));
            --file_count;
            if (current > gone) --current;
            previous_file = -1;
        }
        break;
    case A_STAT: {
        const char *proto = opt.prompt[PROMPT_EQUALS] ? opt.prompt[PROMPT_EQUALS] : default_prompts[PROMPT_EQUALS];
        layout_rows();
        count_lines_now(size);
        expand_prompt(proto, message, sizeof(message));
        break;
    }
    case A_OPT_TOGGLE: option_command(false); break;
    case A_DISP_OPTION: option_command(true); break;
    case A_FIRSTCMD: {
        char cmd[1024] = "";
        if (!simple_prompt("+", cmd, sizeof(cmd), H_NONE)) break;
        free(every_command);
        every_command = *cmd ? strdup(cmd) : NULL;
        break;
    }
    case A_SHELL: case A_PSHELL: {
        if (secure) { say("Command not available"); break; }
        char cmd[4096] = "", expanded[8192];
        if (!simple_prompt(a == A_SHELL ? "!" : "#", cmd, sizeof(cmd), H_SHELL)) break;
        if (a == A_SHELL) expand_command(cmd, expanded, sizeof(expanded), true);
        else expand_prompt(cmd, expanded, sizeof(expanded));
        snprintf(last_shell, sizeof(last_shell), "%s", expanded);
        run_shell(expanded, true);
        break;
    }
    case A_PIPE: if (secure) say("Command not available"); else pipe_command(); break;
    case A_SAVE: if (secure) say("Command not available"); else save_command(); break;
    case A_VISUAL: if (secure) say("Command not available"); else edit_command(); break;
    case A_VERSION: say("%s (less-compatible keys; regex, literal and glob search)", FASTLESS_VERSION); break;
    case A_HELP: show_help(); break;
    case A_QUIT:
        if (viewing_help) close_help();
        else quit(0);
        break;
    case A_X11MOUSE: read_mouse(false); break;
    case A_X116MOUSE: read_mouse(true); break;
    case A_REGEX_MODE: case A_LITERAL_MODE: case A_GLOB_MODE:
        opt.mode = a == A_REGEX_MODE ? REGEX : a == A_LITERAL_MODE ? LITERAL : GLOB;
        if (search_text) {
            char error[256];
            Search s = {0};
            Flags f = search_flags;
            f.no_regex = false;
            if (!compile_search(&s, search_text, f, error, sizeof(error))) { say("%s", error); opt.mode = search.mode; break; }
            clear_search(&search);
            search = s;
            search_flags = f;
        }
        have_hit = false;
        break;
    case A_DIGIT: case A_NOACTION: case A_PREFIX: break;
    case A_INVALID: bell(false); break;
    }
}
/* With -F, prints a file that fits on one screen instead of paging it. */
static bool fits_one_screen(void) {
    dimensions();
    size_t p = settle(body_start());
    if (!size) return true;
    for (int i = 1; i < rows - header_rows; ++i) {
        size_t q;
        if (fwd(p, &q) != 1) return true;
        p = q;
    }
    return false;
}
int pager_run(char **names, int count) {
    secure = getenv("LESSSECURE") && !strcmp(getenv("LESSSECURE"), "1");
    files = calloc((size_t)count, sizeof(File));
    if (!files) return 2;
    for (int i = 0; i < count; ++i) {
        files[i] = (File){.name = strdup(names[i]), .is_stdin = !strcmp(names[i], "-")};
        files[i].in.fd = -1;
    }
    file_count = count;
    apply_render();
    history_load();
    int start = -1;
    for (int i = 0; i < file_count && start < 0; ++i) {
        char error[512] = "";
        if (open_file(i, error, sizeof(error))) start = i;
        else {
            fprintf(stderr, "%s\n", error);
            if (interrupted) return exiting ? 128 + exiting : 130;
        }
    }
    if (start < 0) return 2;
    current = -1;
    switch_file(start);
    if (opt.quit_one_screen && file_count == 1) {
        /* A stream being read fits until it has more than a screen. */
        bool fits;
        while ((fits = fits_one_screen()) && live()) {
            input_demand(&files[current].in, SIZE_MAX);
            await_news();
            if (interrupted) return exiting ? 128 + exiting : 130;
            stream_refresh();
        }
        if (fits) {
            if (message[0]) fprintf(stderr, "%s\n", message);
            print_lines(0, INT_MAX);
            fflush(stdout);
            return 0;
        }
    }
    if (tcgetattr(tty_fd, &saved_term)) { perror("tcgetattr"); return 2; }
    struct sigaction sa;
    sigaction(SIGINT, NULL, &sa);
    sigaction(SIGTSTP, &sa, NULL);
    atexit(restore_at_exit);
    dimensions();
    if (opt.no_init) {
        /* Scroll the existing terminal text up rather than overwrite it. */
        for (int i = 1; i < rows; ++i) putchar('\n');
    }
    enter_screen();
    bool repaint = true;
    while (!quitting) {
        if (exiting) break;
        if (suspended) suspend();
        if (opt.quit_intr && interrupted) { quit(2); break; }
        interrupted = 0;
        resized = 0;
        if (!keys_queued() && repaint) { draw(); count_lines_idle(); }
        repaint = true;
        demand_screen();
        /* -E: the first time the end of the file is on the screen. */
        if (opt.quit_eof == 2 && page_end == SIZE_MAX && !viewing_help && !live()) {
            if (keys_queued()) layout_rows();
            if (page_end == SIZE_MAX) { quit(0); break; }
        }
        double number = 0;
        bool has_number = read_number(&number);
        if (woke) {
            /* More of a stream arrived. The screen changes only where it
               showed the end; otherwise only the status line can. */
            woke = false;
            input_wake_drain();
            bool end_shown = page_end == SIZE_MAX, header_short = header_rows < opt.header_lines;
            repaint = stream_refresh() && (end_shown || header_short || !live() || message[0]);
            if (!repaint && !viewing_help) {
                printf("\033[%d;1H", rows);
                status_line();
                fflush(stdout);
            }
            continue;
        }
        new_file_prompt = false;
        if (exiting) break;
        if (interrupted || resized || suspended) { if (opt.quit_intr && interrupted) { quit(2); break; } continue; }
        char extra[256];
        Action a = read_command(extra, sizeof(extra));
        if (exiting) break;
        if (a == A_NOACTION && interrupted && opt.quit_intr) { quit(2); break; }
        message[0] = 0;
        stream_failure[0] = 0;
        if (a != A_FORW_SCREEN && a != A_FORW_LINE && a != A_FORW_SCROLL && a != A_FORW_WINDOW &&
            a != A_FORW_SCREEN_FORCE)
            unread_line = SIZE_MAX;
        command(a, extra, has_number, number);
        if (opt.quit_intr && interrupted) { quit(2); break; }
    }
    if (exiting) exit_code = 128 + exiting;
    bool redraw = opt.redraw_on_quit && !opt.no_init;
    leave_screen();
    if (redraw) {
        dimensions();
        layout_rows();
        number_rows();
        int width = cols - 1 - number_cols;
        size_t hp = header_begin;
        for (int r = 0; r < header_rows && hp < size; ++r) {
            put_number(header_line ? header_line + (size_t)r : 0);
            LineRequest req = {.begin = hp, .end = line_end(hp), .header_cols = opt.header_cols,
                               .shift = shift, .width = width, .rscroll = opt.rscroll};
            render_line(stdout, &req);
            putchar('\n');
            hp = next_line(hp);
        }
        for (int r = 0; r < body_rows() && row_pos[r] != SIZE_MAX; ++r) {
            put_number(numbers_shown() ? row_line[r] : 0);
            LineRequest req = {.begin = row_pos[r], .end = line_end(row_pos[r]), .header_cols = opt.header_cols,
                               .shift = shift, .width = width, .rscroll = opt.rscroll};
            render_line(stdout, &req);
            putchar('\n');
        }
        fflush(stdout);
    }
    history_save();
    for (int i = 0; i < file_count; ++i) close_document(&files[i]);
    return exit_code;
}

/* ---- Without the pager ---- */
/* Reads a stream to its end before it is searched, showing progress on a
   terminal: 0, or an exit status. */
static int read_whole(InputFile *in, const char *name) {
    bool progress = isatty(STDERR_FILENO), shown = false;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    double last = 0;
    input_demand(in, SIZE_MAX);
    for (;;) {
        char error[512];
        int rc = input_update(in, error, sizeof(error));
        if (rc < 0) { if (shown) fputs("\r\033[K", stderr); fprintf(stderr, "%s\n", error); return 2; }
        if (!in->reading) break;
        await_news();
        if (interrupted) {
            if (shown) fputs("\r\033[K", stderr);
            fprintf(stderr, "%s: Reading interrupted\n", name);
            return exiting ? 128 + exiting : 130;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (progress && elapsed - last >= 0.25) {
            const char *activity = input_activity(in);
            fprintf(stderr, "\r%s %s: %.2f GiB (Ctrl-C cancels)\033[K", name, activity ? activity : "reading",
                    (double)in->size / (1024.0 * 1024.0 * 1024.0));
            fflush(stderr);
            last = elapsed;
            shown = true;
        }
    }
    if (shown) fputs("\r\033[K", stderr);
    return 0;
}
int pager_cat(char **names, int count) {
    int status = 0;
    secure = getenv("LESSSECURE") && !strcmp(getenv("LESSSECURE"), "1");
    cat_mode = true;
    for (int i = 0; i < count; ++i) {
        InputFile in;
        char *replacement = NULL, error[512] = "";
        /* Streams are written directly, after what is buffered. */
        if (fflush(stdout)) { status = 2; perror("stdout"); break; }
        bool is_stdin = !strcmp(names[i], "-");
        InputConfig c = input_config();
        if (is_stdin ? input_copy(STDIN_FILENO, STDOUT_FILENO, "Standard input", &c, error, sizeof(error)) :
            open_document(names[i], false, &in, &replacement, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            status = interrupted ? (exiting ? 128 + exiting : 130) : 2;
            if (interrupted) break;
            continue;
        }
        if (is_stdin) continue;
        if (in.size && fwrite(in.data, 1, in.size, stdout) != in.size) { status = 2; perror("stdout"); }
        input_close(&in);
        free(replacement);
    }
    if (fflush(stdout)) status = 2;
    return status;
}
int pager_find(char **names, int count) {
    if (count != 1) { fputs("Specify exactly one file\n", stderr); return 2; }
    secure = getenv("LESSSECURE") && !strcmp(getenv("LESSSECURE"), "1");
    Search s = {0};
    char error[256] = "";
    if (!set_search(&s, (Mode)opt.mode, opt.find, opt.case_mode, 0, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 2;
    }
    InputFile in;
    char *replacement = NULL;
    bool is_stdin = !strcmp(names[0], "-");
    /* Offsets refer to the file (decoded), never to a preprocessor's output. */
    no_preprocessor = true;
    if (open_document(names[0], is_stdin, &in, &replacement, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return interrupted ? (exiting ? 128 + exiting : 130) : 2;
    }
    if (in.stream) {
        int rc = read_whole(&in, is_stdin ? "Standard input" : names[0]);
        if (rc) { if (!is_stdin) input_close(&in); free(replacement); return rc; }
    }
    data = in.data;
    size = in.size;
    int result = 1;
    size_t offset = opt.offset;
    while (offset <= size) {
        size_t hit;
        int rc = search_forward(&s, offset, size, &hit, error, sizeof(error));
        if (rc < 0) {
            fprintf(stderr, "%s\n", interrupted ? "Search interrupted" : error);
            result = interrupted ? (exiting ? 128 + exiting : 130) : 2;
            break;
        }
        if (!rc) break;
        if (printf("%zu\n", hit) < 0) break;
        result = 0;
        if (!opt.all || hit == size) break;
        offset = s.mode == GLOB ? next_line(hit) : hit + 1;
        if (s.mode == GLOB && offset == size) break;
    }
    if (fflush(stdout) || ferror(stdout)) { perror("stdout"); result = 2; }
    clear_search(&s);
    if (!is_stdin) input_close(&in);
    free(replacement);
    return result;
}

int pager_load_stdin(void) {
    InputConfig c = input_config();
    char error[512];
    if (input_open_fd(&stdin_input, STDIN_FILENO, "standard input", &c, true, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return interrupted ? (exiting ? 128 + exiting : 130) : 2;
    }
    stdin_ready = true;
    return 0;
}
