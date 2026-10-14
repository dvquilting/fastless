#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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
#include <pthread.h>
#include <stdatomic.h>
#include "input.h"
#include "literal.h"

/* No line index and no whole-file heap allocation. All positions are bytes. */
typedef enum { REGEX, LITERAL, GLOB } Mode;
typedef struct {
    Mode mode;
    char *text;
    size_t len;
    Literal fast;
    char *literal; /* owned text of `fast` when it is not the whole pattern */
    bool required, regex_prefix, regex_literal, glob_contains;
    bool fold, compiled;
    regex_t re;
} Search;
static const unsigned char *data;
static size_t size;
static volatile sig_atomic_t interrupted, resized, exiting;
static struct termios saved;
static bool raw;
static int case_mode; /* 0: sensitive, 1: less -i smart case, 2: -I */
static unsigned search_threads = 1;
/* Searches go multi-threaded beyond SEARCH_NEAR bytes, in SEARCH_PIECE units. */
#ifndef SEARCH_NEAR
#define SEARCH_NEAR ((size_t)8 << 20)
#endif
#ifndef SEARCH_PIECE
#define SEARCH_PIECE ((size_t)4 << 20)
#endif
#define SEARCH_STACK ((size_t)8 << 20)
static void signal_handler(int sig) {
    if (sig == SIGWINCH) resized = 1;
    else { interrupted = 1; if (sig == SIGTERM || sig == SIGHUP) exiting = sig; }
}
/* Exit status after a signal: 128 + SIGTERM/SIGHUP, else 130 for SIGINT. */
static int signal_code(void) {
    return exiting ? 128 + (int)exiting : 130;
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
/* Bounds of the line containing p. The last answer is kept so that repeated
   searches inside one very long line do not rescan it each time. */
static void line_bounds(size_t p, size_t *begin, size_t *end) {
    static _Thread_local size_t b, e;
    static _Thread_local bool valid;
    if (!valid || p < b || p > e) { b = line_start(p); e = line_end(p); valid = true; }
    *begin = b; *end = e;
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
    free(s->literal);
    memset(s, 0, sizeof(*s));
}
/* Longest literal that every match of the regex must contain: a run of
   ordinary characters outside groups and brackets, none followed by a
   repetition operator (which may stack, as in "c+?"). Alternation and non-ASCII bytes are not analysed, and
   anything doubtful simply ends a run. The run is text[*from..*to). */
static bool regex_required(const char *text, size_t len, size_t *from, size_t *to) {
    size_t best = 0, run = 0, begin = 0, depth = 0, i = 0;
    *from = *to = 0;
    if (memchr(text, '|', len)) return false;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        size_t next = i + 1;
        bool plain = c < 128 && !strchr(".[]()*+?{}^$\\\n", c);
        if (c == '\\' && next < len && strchr(".[]()*+?{}^$\\", text[next])) { plain = true; ++next; }
        if (plain && !depth && !(next < len && strchr("*+?{", text[next]))) {
            if (!run) begin = i;
            i = next;
            if (++run > best) { best = run; *from = begin; *to = i; }
            continue;
        }
        run = 0;
        if (plain) { i = next; continue; }
        if (c == '\\') ++next;
        else if (c == '(') ++depth;
        else if (c == ')' && depth) --depth;
        else if (c == '{') { while (next < len && text[next] != '}') ++next; }
        else if (c == '[') {
            if (next < len && text[next] == '^') ++next;
            if (next < len && text[next] == ']') ++next;
            while (next < len && text[next] != ']') {
                if (text[next] == '[' && next + 1 < len && strchr(":.=", text[next + 1])) {
                    char kind = text[next + 1];
                    next += 2;
                    while (next + 1 < len && !(text[next] == kind && text[next + 1] == ']')) ++next;
                    next += 2;
                } else ++next;
            }
            ++next;
        }
        i = next;
    }
    return best > 0;
}
/* Longest run of ordinary characters in a glob: every matching line contains
   it. *only is set when the glob is nothing but that run between '*'s. */
static bool glob_required(const char *text, size_t len, size_t *from, size_t *to, bool *only) {
    size_t best = 0, run = 0, runs = 0, begin = 0, i = 0;
    bool other = false;
    *from = *to = 0;
    while (i < len) {
        size_t next = i + 1;
        char c = text[i];
        bool wild = c == '*' || c == '?';
        if (c == '\\' && next < len) c = text[next++];
        if (wild || c == '\n') {
            if (c != '*') other = true;
            run = 0; i = next;
            continue;
        }
        if (!run) { begin = i; ++runs; }
        i = next;
        if (++run > best) { best = run; *from = begin; *to = i; }
    }
    *only = runs == 1 && !other && *from > 0 && *to < len;
    return best > 0;
}
/* Makes text[from..to), with backslash escapes removed, the fast literal. */
static bool set_literal(Search *s, const char *text, size_t from, size_t to) {
    size_t n = 0;
    s->literal = malloc(to - from);
    if (!s->literal) return false;
    for (size_t i = from; i < to; ++i) {
        if (text[i] == '\\' && i + 1 < to) ++i;
        s->literal[n++] = text[i];
    }
    literal_init(&s->fast, s->literal, n, s->fold);
    return true;
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
    size_t from, to;
    if (mode == REGEX) {
        int rc = regcomp(&n.re, text, REG_EXTENDED | (n.fold ? REG_ICASE : 0));
        if (rc) {
            regerror(rc, &n.re, error, cap);
            free(n.text);
            return false;
        }
        n.compiled = true;
        if (regex_required(text, n.len, &from, &to) && set_literal(&n, text, from, to)) {
            n.required = true;
            n.regex_prefix = from == (text[0] == '^' ? 1u : 0u);
            n.regex_literal = from == 0 && to == n.len;
        }
    } else if (mode == LITERAL) {
        literal_init(&n.fast, n.text, n.len, n.fold);
    } else {
        bool only;
        if (glob_required(text, n.len, &from, &to, &only) && set_literal(&n, text, from, to)) {
            n.required = true;
            n.glob_contains = only;
        }
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
        if (s->fold) { c = literal_lower(c); d = literal_lower(d); }
        if (p < s->len && (any || c == d)) { p += step; ++t; }
        else if (star != SIZE_MAX) { p = star; t = ++retry; }
        else return false;
    }
    while (p < s->len && s->text[p] == '*') ++p;
    return p == s->len;
}
/* Single-threaded search of [start, limit).
   Returns 1 for a match, 0 for no match, -1 for cancellation/error. */
static int forward_scan(const Search *s, size_t start, size_t limit,
                        size_t *hit, char *error, size_t cap) {
    if (start > limit || limit > size) return 0;
    /* A regex that is a plain literal needs neither line bounds nor regexec. */
    if (s->mode == LITERAL || s->regex_literal) {
        const unsigned char *q = literal_find(&s->fast, data + start, limit - start, &interrupted);
        if (q) { *hit = (size_t)(q - data); return 1; }
        return interrupted ? -1 : 0;
    }
    size_t at = start;
    while (at <= limit) {
        if (interrupted) return -1;
        /* The literal every match must contain (never a newline) is searched
           for across the whole range, so lines without it are not visited. */
        if (s->required) {
            const unsigned char *q = literal_find(&s->fast, data + at, limit - at, &interrupted);
            if (!q) break;
            at = (size_t)(q - data);
        }
        size_t p, end;
        line_bounds(at, &p, &end);
        if (p >= size && size) break;
        if (end > limit) end = limit;
        if (s->mode == GLOB) {
            if (p >= start && (s->glob_contains || glob_match(s, p, end))) { *hit = p; return 1; }
        } else {
            /* POSIX regoff_t is typically signed 32-bit, even on 64-bit OSes. */
            if (end - p > INT_MAX) {
                snprintf(error, cap, "Regex line exceeds 2 GiB; use literal or glob mode");
                return -1;
            }
            /* Only a literal that begins the pattern marks where a match starts. */
            size_t from = s->regex_prefix ? at : start > p ? start : p;
            regmatch_t match = {.rm_so = (regoff_t)(from - p),
                                .rm_eo = (regoff_t)(end - p)};
            int rc = regexec(&s->re, (const char *)data + p, 1, &match,
                             REG_STARTEND | (from > p ? REG_NOTBOL : 0));
            if (!rc) { *hit = p + (size_t)match.rm_so; return 1; }
            if (rc != REG_NOMATCH) { regerror(rc, &s->re, error, cap); return -1; }
        }
        if (end >= limit || end >= size) break;
        at = end + 1;
    }
    return interrupted ? -1 : 0;
}
/* Single-threaded search for the last match starting before `before`. Literal
   matches must start at or after `floor`; line-based ones must lie on a line
   starting at or after it. */
static int backward_scan(const Search *s, size_t before, size_t floor,
                         size_t *hit, char *error, size_t cap) {
    if (before <= floor) return 0;
    if (s->mode == LITERAL || s->regex_literal) {
        const unsigned char *q = literal_reverse(&s->fast, data + floor, size - floor,
                                                 before - floor, &interrupted);
        if (q) { *hit = (size_t)(q - data); return 1; }
        return interrupted ? -1 : 0;
    }
    size_t p, end;
    line_bounds(before - 1, &p, &end);
    while (p >= floor) {
        size_t cursor = p, best = 0;
        bool found = false;
        while (cursor <= end && cursor < before) {
            size_t h;
            int rc = forward_scan(s, cursor, end, &h, error, cap);
            if (rc < 0) return rc;
            if (!rc || h >= before) break;
            best = h; found = true;
            if (s->mode == GLOB || h == end) break;
            cursor = h + 1;
        }
        if (found) { *hit = best; return 1; }
        if (p <= floor) break;
        size_t earlier = p - 1;
        if (s->required) {
            /* Skip straight back to the nearest line holding the literal. */
            const unsigned char *q = literal_reverse(&s->fast, data + floor, size - floor,
                                                     p - floor, &interrupted);
            if (!q) break;
            earlier = (size_t)(q - data);
        }
        line_bounds(earlier, &p, &end);
    }
    return interrupted ? -1 : 0;
}
/* A piece [a, b) of a larger search owns the literal matches starting in it
   or, for line-based modes, the matches on lines starting in it, so pieces
   can be searched independently and their results ordered by position.
   `first` marks the piece at the search start, which may begin mid-line. */
static int forward_piece(const Search *s, size_t a, size_t b, size_t limit, bool first,
                         size_t *hit, char *error, size_t cap) {
    if (s->mode == LITERAL || s->regex_literal) {
        if (limit - b > s->fast.len - 1) limit = b + s->fast.len - 1;
        return forward_scan(s, a, limit, hit, error, cap);
    }
    if (!first && a && data[a - 1] != '\n') a = next_line(a);
    if (b < limit) { b = line_end(b - 1); if (b > limit) b = limit; }
    return a > b ? 0 : forward_scan(s, a, b, hit, error, cap);
}
static int backward_piece(const Search *s, size_t a, size_t b, size_t before,
                          size_t *hit, char *error, size_t cap) {
    if (s->mode != LITERAL && !s->regex_literal) b = line_end(b - 1) + 1;
    return backward_scan(s, b < before ? b : before, a, hit, error, cap);
}
typedef struct {
    const Search *s;
    bool forward;
    size_t low, high, bound, count; /* pieces of [low, high); limit or `before` */
    atomic_size_t next, best;       /* next piece to hand out; first piece with a result */
    pthread_mutex_t lock;
    int rc;
    size_t hit;
    char error[256];
} Parallel;
static void *search_worker(void *arg) {
    Parallel *job = arg;
    Search local = *job->s;
    /* glibc serializes regexec on one compiled pattern, so each thread
       compiles its own copy; if that fails it shares the original. */
    bool own = local.mode == REGEX && !local.regex_literal &&
               !regcomp(&local.re, local.text, REG_EXTENDED | (local.fold ? REG_ICASE : 0));
    if (!own) local.re = job->s->re;
    for (;;) {
        /* Pieces are numbered in search order, so the lowest-numbered result
           wins and nothing beyond it needs starting. */
        size_t i = atomic_fetch_add(&job->next, 1);
        if (i >= job->count || i > atomic_load(&job->best) || interrupted) break;
        size_t a, b, hit = 0;
        char error[256] = "";
        int rc;
        if (job->forward) {
            a = job->low + i * SEARCH_PIECE;
            b = job->high - a > SEARCH_PIECE ? a + SEARCH_PIECE : job->high;
            rc = forward_piece(&local, a, b, job->bound, false, &hit, error, sizeof(error));
        } else {
            b = job->high - i * SEARCH_PIECE;
            a = b - job->low > SEARCH_PIECE ? b - SEARCH_PIECE : job->low;
            rc = backward_piece(&local, a, b, job->bound, &hit, error, sizeof(error));
        }
        if (!rc || interrupted) continue;
        pthread_mutex_lock(&job->lock);
        if (i < atomic_load(&job->best)) {
            atomic_store(&job->best, i);
            job->rc = rc; job->hit = hit;
            memcpy(job->error, error, sizeof(error));
        }
        pthread_mutex_unlock(&job->lock);
    }
    if (own) regfree(&local.re);
    return NULL;
}
/* Searches the pieces of [low, high) on all search threads. */
static int search_pieces(const Search *s, bool forward, size_t low, size_t high, size_t bound,
                         size_t *hit, char *error, size_t cap) {
    Parallel job = {.s = s, .forward = forward, .low = low, .high = high, .bound = bound,
                    .count = (high - low) / SEARCH_PIECE + ((high - low) % SEARCH_PIECE != 0),
                    .lock = PTHREAD_MUTEX_INITIALIZER};
    atomic_init(&job.next, 0);
    atomic_init(&job.best, SIZE_MAX);
    pthread_t threads[256];
    unsigned started = 0;
    /* Give workers a main-thread-sized stack for regexec; macOS defaults
       secondary threads to 512 KiB. */
    pthread_attr_t attr;
    bool sized = !pthread_attr_init(&attr);
    if (sized && pthread_attr_setstacksize(&attr, SEARCH_STACK)) {
        pthread_attr_destroy(&attr);
        sized = false;
    }
    while (started + 1 < search_threads && started + 1 < job.count &&
           !pthread_create(&threads[started], sized ? &attr : NULL, search_worker, &job)) ++started;
    if (sized) pthread_attr_destroy(&attr);
    search_worker(&job);
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    if (interrupted) return -1;
    if (job.rc == 1) *hit = job.hit;
    else if (job.rc < 0) snprintf(error, cap, "%s", job.error);
    return job.rc;
}
/* The part nearest the starting point is searched on this thread, so nearby
   matches cost no thread start-up; the rest is divided among all threads. */
static int search_forward(const Search *s, size_t start, size_t limit,
                          size_t *hit, char *error, size_t cap) {
    if (start > limit || limit > size) return 0;
    if (search_threads < 2 || limit - start <= SEARCH_NEAR)
        return forward_scan(s, start, limit, hit, error, cap);
    int rc = forward_piece(s, start, start + SEARCH_NEAR, limit, true, hit, error, cap);
    return rc ? rc : search_pieces(s, true, start + SEARCH_NEAR, limit, limit, hit, error, cap);
}
static int search_backward(const Search *s, size_t before, size_t *hit, char *error, size_t cap) {
    if (before > size) before = size;
    if (search_threads < 2 || before <= SEARCH_NEAR)
        return backward_scan(s, before, 0, hit, error, cap);
    int rc = backward_piece(s, before - SEARCH_NEAR, before, before, hit, error, cap);
    return rc ? rc : search_pieces(s, false, 0, before - SEARCH_NEAR, before, hit, error, cap);
}
static void less_options(void) {
    const char *p = getenv("LESS");
    if (!p) return;
    /* As in less: the leading '-' is optional, flags may be clustered, and a
       string-valued option (or +command) runs to the next '$' or the end. */
    while (*p) {
        char c = *p++;
        if (c == '-' && *p == '-') {
            const char *name = ++p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            size_t n = (size_t)(p - name);
            if (n == 11 && !memcmp(name, "ignore-case", n)) case_mode = 1;
            else if (n == 11 && !memcmp(name, "IGNORE-CASE", n)) case_mode = 2;
        } else if (strchr("+DkoOpPtT\"", c)) {
            while (*p && *p != '$') ++p;
            if (*p) ++p;
        } else if (c == 'i') case_mode = 1;
        else if (c == 'I') case_mode = 2;
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
    } else if (s->regex_literal) {
        /* Only matches starting in the visible part matter. */
        if (end - visible_end > s->fast.len - 1) limit = visible_end + s->fast.len - 1;
    } else if (end - begin > INT_MAX) return;
    while (cursor < visible_end && !interrupted) {
        size_t a, b;
        if (s->mode == LITERAL) {
            char error[1];
            if (search_forward(s, cursor, limit, &a, error, sizeof(error)) != 1) break;
            b = a + s->len;
        } else if (s->regex_literal) {
            const unsigned char *q = literal_find(&s->fast, data + cursor, limit - cursor, &interrupted);
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
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT); sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP); sigaddset(&block, SIGWINCH);
    for (;;) {
        /* Signals are held off between testing the flags and waiting, and let
           in only by pselect, so one that arrived earlier (say during draw)
           cannot be left unnoticed until the next keypress. */
        sigprocmask(SIG_BLOCK, &block, &old);
        bool pending = interrupted || resized || exiting;
        int ready = 0, saved_errno = 0;
        if (!pending) {
            fd_set in;
            FD_ZERO(&in); FD_SET(STDIN_FILENO, &in);
            ready = pselect(STDIN_FILENO + 1, &in, NULL, NULL, NULL, &old);
            saved_errno = errno;
        }
        sigprocmask(SIG_SETMASK, &old, NULL);
        if (pending) return 0;
        if (ready < 0) { if (saved_errno != EINTR) return -1; continue; }
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1) return c;
        if (!n || (n < 0 && errno != EINTR)) return -1;
    }
}
static bool prompt(char *out, size_t cap, int *rows, char prefix) {
    size_t n = 0;
    for (;;) {
        printf("\033[%d;1H\033[2K%c%.*s", *rows, prefix, (int)n, out);
        fflush(stdout);
        int c = key();
        if (c == '\r' || c == '\n') { out[n] = 0; return true; }
        if (c == 27 || c < 0 || interrupted || exiting) return false;
        if (resized) { int cols; resized = 0; dimensions(rows, &cols); }
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
          "--threads N: search and xz decoder threads (0: auto, at most 32; maximum: 256).\n"
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
    if (!threads) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        threads = cpus > 0 ? (unsigned)(cpus > 32 ? 32 : cpus) : 1;
    }
    search_threads = threads;
    if (input_open(&input, file, compression, temp_dir, threads, &interrupted, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        result = interrupted ? signal_code() : 2;
        goto done;
    }
    data = input.data; size = input.size;
    if (pattern) {
        bool found = false;
        while (offset <= size) {
            size_t hit;
            int rc = search_forward(&s, offset, size, &hit, error, sizeof(error));
            if (rc < 0) { fprintf(stderr, "%s\n", interrupted ? "Search interrupted" : error); result = interrupted ? signal_code() : 2; goto done; }
            if (!rc) break;
            if (printf("%zu\n", hit) < 0) break;
            found = true;
            if (!all || hit == size) break;
            offset = mode == GLOB ? next_line(hit) : hit + 1;
            if (mode == GLOB && offset == size) break;
        }
        if (fflush(stdout) || ferror(stdout)) { perror("stdout"); result = 2; goto done; }
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
                if (!prompt(input, sizeof(input), &rows, (char)c)) continue;
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
    if (exiting) result = signal_code();
done:
    clear_search(&s);
    input_close(&input);
    return result;
}
