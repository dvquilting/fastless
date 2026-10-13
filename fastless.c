#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <regex.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include "input.h"
#include "literal.h"

/* No line index and no whole-file heap allocation. All positions are bytes. */
typedef enum { REGEX, LITERAL, GLOB } Mode;
typedef struct {
    Mode mode;
    char *text;
    size_t len;
    Literal fast;
    bool regex_prefix, regex_literal, glob_contains;
    bool fold, compiled;
    regex_t re;
} Search;
static const unsigned char *data;
static size_t size;
static volatile sig_atomic_t interrupted, resized, exiting;
static struct termios saved;
static bool raw;
static int case_mode; /* 0: sensitive, 1: less -i smart case, 2: -I */
static unsigned char lower_byte(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}
static void signal_handler(int sig) {
    if (sig == SIGWINCH) resized = 1;
    else { interrupted = 1; if (sig == SIGTERM || sig == SIGHUP) exiting = sig; }
}
static void restore(void) {
    if (raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        fputs("\033[?1049l", stdout);
        fflush(stdout);
        raw = false;
    }
}
static size_t line_end(size_t p) {
    if (p >= size) return size;
    const unsigned char *q = memchr(data + p, '\n', size - p);
    return q ? (size_t)(q - data) : size;
}
static size_t line_start(size_t p) {
    if (p > size) p = size;
#ifdef __linux__
    const unsigned char *q = memrchr(data, '\n', p);
    return q ? (size_t)(q - data) + 1 : 0;
#else
    while (p && data[p - 1] != '\n') --p;
    return p;
#endif
}
static size_t next_line(size_t p) {
    size_t e = line_end(p);
    return e < size ? e + 1 : size;
}
static size_t previous_line(size_t p) {
    return p ? line_start(p - 1) : 0;
}
static void clear_search(Search *s) {
    if (s->compiled) regfree(&s->re);
    free(s->text);
    memset(s, 0, sizeof(*s));
}
static bool set_search(Search *s, Mode mode, const char *text, char *error, size_t cap) {
    Search n = {.mode = mode, .len = strlen(text)};
    if (!n.len) { snprintf(error, cap, "Empty pattern"); return false; }
    n.text = strdup(text);
    if (!n.text) { snprintf(error, cap, "Out of memory"); return false; }
    bool uppercase = false;
    for (size_t i = 0; i < n.len; ++i)
        if (text[i] >= 'A' && text[i] <= 'Z') uppercase = true;
    n.fold = case_mode == 2 || (case_mode == 1 && !uppercase);
    if (mode == REGEX) {
        int rc = regcomp(&n.re, text, REG_EXTENDED | (n.fold ? REG_ICASE : 0));
        if (rc) {
            regerror(rc, &n.re, error, cap);
            free(n.text);
            return false;
        }
        n.compiled = true;
        /* A literal prefix outside groups is required by every match, provided
           there is no alternation and its last byte isn't optionally repeated.
           Conservatively reject ambiguous syntax and non-ASCII prefixes. */
        if (!strchr(text, '|')) {
            size_t start = text[0] == '^' ? 1 : 0, end = start;
            while (end < n.len && (unsigned char)text[end] < 128 &&
                   !strchr(".[\\]()*+?{}^$", text[end])) ++end;
            if (end > start && (end == n.len || !strchr("*+?{", text[end]))) {
                literal_init(&n.fast, n.text + start, end - start, n.fold);
                n.regex_prefix = true;
                n.regex_literal = start == 0 && end == n.len;
            }
        }
    } else if (mode == LITERAL) {
        literal_init(&n.fast, n.text, n.len, n.fold);
    } else if (n.len > 2 && text[0] == '*' && text[n.len - 1] == '*') {
        n.glob_contains = true;
        for (size_t i = 1; i + 1 < n.len; ++i)
            if (strchr("*?\\", text[i])) n.glob_contains = false;
        if (n.glob_contains) literal_init(&n.fast, n.text + 1, n.len - 2, n.fold);
    }
    clear_search(s);
    *s = n;
    return true;
}
/* Greedy whole-line glob; '*' and '?' are the only metacharacters.
   Backslash quotes the following character. No temporary line buffer. */
static bool glob_match(const Search *s, size_t a, size_t b) {
    if (s->glob_contains) return literal_find(&s->fast, data + a, b - a, &interrupted) != NULL;
    size_t p = 0, t = a, star = SIZE_MAX, retry = a, ticks = 0;
    while (t < b) {
        if (!(++ticks & 65535) && interrupted) return false;
        if (p < s->len && s->text[p] == '*') { star = ++p; retry = t; continue; }
        size_t step = 1;
        unsigned char c = p < s->len ? (unsigned char)s->text[p] : 0;
        bool any = c == '?';
        if (c == '\\' && p + 1 < s->len) { c = (unsigned char)s->text[p + 1]; step = 2; any = false; }
        unsigned char d = data[t];
        if (s->fold) { c = lower_byte(c); d = lower_byte(d); }
        if (p < s->len && (any || c == d)) { p += step; ++t; }
        else if (star != SIZE_MAX) { p = star; t = ++retry; }
        else return false;
    }
    while (p < s->len && s->text[p] == '*') ++p;
    return p == s->len;
}
/* Returns 1 for a match, 0 for no match, -1 for cancellation/error. */
static int search_forward(const Search *s, size_t start, size_t limit,
                          size_t *hit, char *error, size_t cap) {
    if (start > limit || limit > size) return 0;
    if (s->mode == LITERAL) {
        const unsigned char *q = literal_find(&s->fast, data + start, limit - start, &interrupted);
        if (q) { *hit = (size_t)(q - data); return 1; }
        return interrupted ? -1 : 0;
    }
    size_t p = line_start(start);
    while (p <= limit && (p < size || !size)) {
        if (interrupted) return -1;
        size_t end = line_end(p);
        if (end > limit) end = limit;
        if (s->mode == GLOB) {
            if (p >= start && glob_match(s, p, end)) { *hit = p; return 1; }
        } else {
            /* POSIX regoff_t is typically signed 32-bit, even on 64-bit OSes. */
            if (end - p > INT_MAX) {
                snprintf(error, cap, "Regex line exceeds 2 GiB; use literal or glob mode");
                return -1;
            }
            size_t from = start > p ? start : p;
            if (s->regex_prefix) {
                const unsigned char *q = literal_find(&s->fast, data + from, end - from, &interrupted);
                if (!q) goto next_search_line;
                from = (size_t)(q - data);
                if (s->regex_literal) { *hit = from; return 1; }
            }
            regmatch_t match = {.rm_so = (regoff_t)(from - p),
                                .rm_eo = (regoff_t)(end - p)};
            int rc = regexec(&s->re, (const char *)data + p, 1, &match,
                             REG_STARTEND | (from > p ? REG_NOTBOL : 0));
            if (!rc) { *hit = p + (size_t)match.rm_so; return 1; }
            if (rc != REG_NOMATCH) { regerror(rc, &s->re, error, cap); return -1; }
        }
next_search_line:
        if (end >= limit || end >= size) break;
        p = end + 1;
    }
    return interrupted ? -1 : 0;
}
static int search_backward(const Search *s, size_t before, size_t *hit, char *error, size_t cap) {
    if (!before) return 0;
    if (s->mode == LITERAL) {
        const unsigned char *q = literal_reverse(&s->fast, data, size, before, &interrupted);
        if (q) { *hit = (size_t)(q - data); return 1; }
        return interrupted ? -1 : 0;
    }
    size_t p = line_start(before - 1);
    for (;;) {
        size_t end = line_end(p), cursor = p, best = 0;
        bool found = false;
        while (cursor <= end && cursor < before) {
            size_t h;
            int rc = search_forward(s, cursor, end, &h, error, cap);
            if (rc < 0) return rc;
            if (!rc || h >= before) break;
            best = h; found = true;
            if (s->mode == GLOB || h == end) break;
            cursor = h + 1;
        }
        if (found) { *hit = best; return 1; }
        if (!p) break;
        p = previous_line(p);
    }
    return 0;
}
static void less_options(void) {
    const char *p = getenv("LESS");
    if (!p) return;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        const char *end = p;
        while (*end && !isspace((unsigned char)*end)) ++end;
        if (*p == '-' && p + 1 < end && p[1] != '-') {
            for (const char *q = p + 1; q < end; ++q) {
                if (*q == 'i') case_mode = 1;
                if (*q == 'I') case_mode = 2;
            }
        }
        p = end;
    }
}
static void dimensions(int *rows, int *cols) {
    struct winsize w;
    *rows = 24; *cols = 80;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)) {
        if (w.ws_row >= 3) *rows = w.ws_row;
        if (w.ws_col >= 10) *cols = w.ws_col;
    }
}
/* One byte per visible input byte, independent of file/line size. Regexes
   still see the complete line so clipping does not change anchors or greediness. */
static void highlight_line(const Search *s, size_t begin, size_t end,
                           size_t visible_end, unsigned char *marks) {
    memset(marks, 0, visible_end - begin);
    if (!s->text || begin == visible_end || interrupted) return;
    if (s->mode == GLOB) {
        if (glob_match(s, begin, end)) memset(marks, 1, visible_end - begin);
        return;
    }
    size_t cursor = begin, limit = end;
    if (s->mode == LITERAL) {
        /* Include matches crossing into/out of this line, even across newlines. */
        cursor = begin >= s->len - 1 ? begin - (s->len - 1) : 0;
        limit = s->len - 1 > size - visible_end ? size : visible_end + s->len - 1;
    } else if (end - begin > INT_MAX) return;
    while (cursor < visible_end && !interrupted) {
        size_t a, b;
        if (s->mode == LITERAL) {
            char error[1];
            if (search_forward(s, cursor, limit, &a, error, sizeof(error)) != 1) break;
            b = a + s->len;
        } else if (s->regex_literal) {
            const unsigned char *q = literal_find(&s->fast, data + cursor, end - cursor, &interrupted);
            if (!q) break;
            a = (size_t)(q - data); b = a + s->fast.len;
        } else {
            regmatch_t match = {.rm_so = (regoff_t)(cursor - begin),
                                .rm_eo = (regoff_t)(end - begin)};
            if (regexec(&s->re, (const char *)data + begin, 1, &match,
                        REG_STARTEND | (cursor > begin ? REG_NOTBOL : 0))) break;
            a = begin + (size_t)match.rm_so;
            b = begin + (size_t)match.rm_eo;
        }
        if (a >= visible_end) break;
        size_t lo = a < begin ? begin : a;
        size_t hi = b > visible_end ? visible_end : b;
        if (hi > lo) memset(marks + lo - begin, 1, hi - lo);
        /* Include overlaps; zero-length regex matches advance without painting. */
        cursor = a + 1;
    }
}
static size_t draw(size_t top, const Search *s, const char *message, int rows, int cols) {
    unsigned char *marks = s->text ? malloc((size_t)cols) : NULL;
    fputs("\033[0m\033[H", stdout);
    size_t p = top;
    for (int row = 0; row < rows - 1; ++row) {
        fputs("\033[2K", stdout);
        if (p < size) {
            size_t end = line_end(p), q = p;
            int col = 0;
            /* Count visible bytes first: tabs consume several screen columns. */
            while (q < end && col < cols - 1) {
                unsigned char c = data[q++];
                col = c == '\t' ? (col + 8) & ~7 : col + 1;
            }
            size_t visible_end = q;
            if (marks) highlight_line(s, p, end, visible_end, marks);
            q = p; col = 0;
            bool highlighted = false;
            while (q < end && col < cols - 1) {
                bool matched = marks && marks[q - p];
                if (matched != highlighted) {
                    fputs(matched ? "\033[7m" : "\033[0m", stdout);
                    highlighted = matched;
                }
                unsigned char c = data[q++];
                if (c == '\t') {
                    int stop = (col + 8) & ~7;
                    while (col < stop && col < cols - 1) { putchar(' '); ++col; }
                } else { putchar(c >= 32 && c <= 126 ? c : '.'); ++col; }
            }
            if (highlighted) fputs("\033[0m", stdout);
            p = end < size ? end + 1 : size;
        } else putchar('~');
        fputs("\r\n", stdout);
    }
    const char *mode = s->mode == REGEX ? "regex" : s->mode == LITERAL ? "literal" : "glob";
    char status[512];
    snprintf(status, sizeof(status), "%s | byte %zu/%zu (%.1f%%) | %s%s | %s",
             "fastless", top, size, size ? 100.0 * (double)top / (double)size : 100.0,
             mode, s->fold ? ", ignore case" : "", message);
    fputs("\033[7m\033[2K", stdout);
    printf("%.*s", cols - 1, status);
    fputs("\033[0m", stdout);
    fflush(stdout);
    free(marks);
    return p;
}
static int key(void) {
    unsigned char c;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1) return c;
        if (!n || (n < 0 && errno != EINTR)) return -1;
        if (interrupted || resized) return 0;
    }
}
static bool prompt(char *out, size_t cap, int rows, char prefix) {
    size_t n = 0;
    for (;;) {
        printf("\033[%d;1H\033[2K%c%.*s", rows, prefix, (int)n, out);
        fflush(stdout);
        int c = key();
        if (c == '\r' || c == '\n') { out[n] = 0; return true; }
        if (c == 27 || c < 0 || interrupted) return false;
        if (c == 127 || c == 8) { if (n) --n; }
        else if (c >= 32 && c < 127 && n + 1 < cap) out[n++] = (char)c;
    }
}
static void usage(FILE *out) {
    fputs("Usage: fastless [--regex|--literal|--glob] [-i|-I|--case-sensitive] FILE\n"
          "       fastless [options] --find PATTERN [--all] [--offset BYTE] FILE\n"
          "Search: POSIX extended regex (default), literal bytes, or whole-line glob (* ? \\).\n"
          "Keys: Space/f next page, b previous page, j/k line, g/G start/end,\n"
          "      / forward search, ? backward search, n/N repeat/reverse,\n"
          "      r/l/w regex/literal/glob mode, q quit. Ctrl-C cancels a search.\n"
          "-i: ignore ASCII case for lowercase patterns; -I: always ignore case.\n"
          "LESS -i/-I supplies defaults; command-line options override them.\n"
          "gzip, zstd, and xz are detected by magic bytes and decompressed to a\n"
          "temporary file before paging/searching. Offsets refer to decoded bytes.\n"
          "--tmpdir DIR: temporary storage (default: TMPDIR or /tmp).\n"
          "--compression auto|none|gzip|zstd|xz: force input format.\n"
          "--threads N: xz decoder threads (0: auto, at most 32; maximum: 256).\n"
          "--simd auto|off|avx2: runtime search acceleration (default: auto).\n"
          "--find prints matching byte offsets without opening the pager.\n", out);
}
int main(int argc, char **argv) {
    Mode mode = REGEX;
    const char *file = NULL, *pattern = NULL;
    const char *temp_dir = NULL, *simd = "auto";
    Compression compression = COMP_AUTO;
    unsigned threads = 0;
    bool all = false, positional = false;
    size_t offset = 0;
    less_options();
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!positional && !strcmp(a, "--")) positional = true;
        else if (!positional && (!strcmp(a, "--help") || !strcmp(a, "-h"))) { usage(stdout); return 0; }
        else if (!positional && !strcmp(a, "--literal")) mode = LITERAL;
        else if (!positional && !strcmp(a, "--regex")) mode = REGEX;
        else if (!positional && !strcmp(a, "--glob")) mode = GLOB;
        else if (!positional && !strcmp(a, "-i")) case_mode = 1;
        else if (!positional && !strcmp(a, "-I")) case_mode = 2;
        else if (!positional && !strcmp(a, "--case-sensitive")) case_mode = 0;
        else if (!positional && !strcmp(a, "--all")) all = true;
        else if (!positional && !strcmp(a, "--find") && i + 1 < argc) pattern = argv[++i];
        else if (!positional && !strcmp(a, "--tmpdir") && i + 1 < argc) temp_dir = argv[++i];
        else if (!positional && !strcmp(a, "--simd") && i + 1 < argc) simd = argv[++i];
        else if (!positional && !strcmp(a, "--compression") && i + 1 < argc) {
            const char *format = argv[++i];
            if (!strcmp(format, "auto")) compression = COMP_AUTO;
            else if (!strcmp(format, "none")) compression = COMP_NONE;
            else if (!strcmp(format, "gzip")) compression = COMP_GZIP;
            else if (!strcmp(format, "zstd")) compression = COMP_ZSTD;
            else if (!strcmp(format, "xz")) compression = COMP_XZ;
            else { fputs("Invalid compression format\n", stderr); return 2; }
        } else if (!positional && !strcmp(a, "--threads") && i + 1 < argc) {
            char *end;
            const char *v = argv[++i];
            errno = 0;
            unsigned long n = strtoul(v, &end, 10);
            if (!*v || *v == '-' || *end || errno || n > 256) {
                fputs("Threads must be between 0 and 256\n", stderr); return 2;
            }
            threads = (unsigned)n;
        }
        else if (!positional && !strcmp(a, "--offset") && i + 1 < argc) {
            char *end;
            const char *v = argv[++i];
            errno = 0;
            unsigned long long n = strtoull(v, &end, 10);
            if (!*v || *v == '-' || *end || errno || n > SIZE_MAX) {
                fputs("Invalid byte offset\n", stderr); return 2;
            }
            offset = (size_t)n;
        } else if (!positional && a[0] == '-') { usage(stderr); return 2; }
        else if (file) { fputs("Specify exactly one file\n", stderr); return 2; }
        else file = a;
    }
    if (!file || (all && !pattern)) { usage(stderr); return 2; }
    if (!literal_simd_configure(simd)) {
        fputs("Invalid SIMD mode or AVX2 unavailable; use auto, off, or avx2\n", stderr); return 2;
    }
    if (sizeof(void *) < 8) { fputs("A 64-bit build is required\n", stderr); return 2; }
    if (!pattern && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fputs("Pager needs a terminal; use --find for noninteractive searching\n", stderr); return 2;
    }
    InputFile input = {0};
    Search s = {.mode = mode};
    char message[256] = "q quit; / search; r/l/w mode", error[256] = "";
    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
    int result = 0;
    if (pattern && !set_search(&s, mode, pattern, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error); result = 2; goto done;
    }
    if (input_open(&input, file, compression, temp_dir, threads, &interrupted, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        result = interrupted ? (exiting ? 128 + exiting : 130) : 2;
        goto done;
    }
    data = input.data; size = input.size;
    if (pattern) {
        bool found = false;
        while (offset <= size) {
            size_t hit;
            int rc = search_forward(&s, offset, size, &hit, error, sizeof(error));
            if (rc < 0) { fprintf(stderr, "%s\n", interrupted ? "Search interrupted" : error); result = interrupted ? (exiting ? 128 + exiting : 130) : 2; goto done; }
            if (!rc) break;
            printf("%zu\n", hit); found = true;
            if (!all || hit == size) break;
            offset = mode == GLOB ? next_line(hit) : hit + 1;
            if (mode == GLOB && offset == size) break;
        }
        result = found ? 0 : 1;
        goto done;
    }
    if (tcgetattr(STDIN_FILENO, &saved)) { perror("tcgetattr"); result = 2; goto done; }
    struct termios term = saved;
    term.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    term.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    term.c_cc[VMIN] = 1; term.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term)) { perror("tcsetattr"); result = 2; goto done; }
    raw = true; atexit(restore);
    fputs("\033[?1049h", stdout);
    size_t top = line_start(offset), last_hit = 0;
    bool have_hit = false;
    int direction = 1;
    for (;;) {
        if (exiting) break;
        interrupted = 0; resized = 0;
        int rows, cols;
        dimensions(&rows, &cols);
        size_t page_end = draw(top, &s, message, rows, cols);
        int c = key();
        if (c < 0 || c == 'q' || exiting) break;
        if (c == ' ' || c == 'f') { if (page_end < size) top = page_end; have_hit = false; }
        else if (c == 'b') { for (int i = 0; i < rows - 1; ++i) top = previous_line(top); have_hit = false; }
        else if (c == 'j' || c == '\n' || c == '\r') { size_t p = next_line(top); if (p < size) top = p; have_hit = false; }
        else if (c == 'k') { top = previous_line(top); have_hit = false; }
        else if (c == 'g') { top = 0; have_hit = false; }
        else if (c == 'G') { top = size ? line_start(size - (data[size - 1] == '\n' ? 1 : 0)) : 0; for (int i = 1; i < rows - 1; ++i) top = previous_line(top); have_hit = false; }
        else if (c == 'r' || c == 'l' || c == 'w') {
            Mode m = c == 'r' ? REGEX : c == 'l' ? LITERAL : GLOB;
            if (s.text) { char *copy = strdup(s.text); if (copy) { if (!set_search(&s, m, copy, error, sizeof(error))) snprintf(message, sizeof(message), "%s", error); free(copy); } }
            else s.mode = m;
            have_hit = false;
        } else if (c == '/' || c == '?' || c == 'n' || c == 'N') {
            bool fresh = c == '/' || c == '?';
            if (fresh) {
                char input[4096] = "";
                if (!prompt(input, sizeof(input), rows, (char)c)) continue;
                if (*input && !set_search(&s, s.mode, input, error, sizeof(error))) { snprintf(message, sizeof(message), "%s", error); continue; }
                direction = c == '/' ? 1 : -1;
            }
            if (!s.text) { snprintf(message, sizeof(message), "No search pattern"); continue; }
            int dir = c == 'N' ? -direction : direction;
            size_t start = fresh || !have_hit ? top : last_hit;
            if (!fresh && have_hit && dir > 0) {
                if (start == size) { snprintf(message, sizeof(message), "No further match"); continue; }
                start = s.mode == GLOB ? next_line(start) : start + 1;
            }
            size_t hit;
            error[0] = 0;
            printf("\033[%d;1H\033[2KSearching... (Ctrl-C cancels)", rows); fflush(stdout);
            int rc = dir > 0 ? search_forward(&s, start, size, &hit, error, sizeof(error)) : search_backward(&s, start, &hit, error, sizeof(error));
            if (rc == 1) { last_hit = hit; have_hit = true; top = line_start(hit); snprintf(message, sizeof(message), "Match at byte %zu", hit); }
            else snprintf(message, sizeof(message), "%s", interrupted ? "Search cancelled" : rc < 0 ? error : "Pattern not found (no wrap)");
        }
    }
    restore();
    if (exiting) result = 128 + exiting;
done:
    clear_search(&s);
    input_close(&input);
    return result;
}
