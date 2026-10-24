#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "search.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>

const unsigned char *data;
size_t size;
volatile sig_atomic_t interrupted;
unsigned search_threads = 1;
/* Searches go multi-threaded beyond SEARCH_NEAR bytes, in SEARCH_PIECE units. */
#ifndef SEARCH_NEAR
#define SEARCH_NEAR ((size_t)8 << 20)
#endif
#ifndef SEARCH_PIECE
#define SEARCH_PIECE ((size_t)4 << 20)
#endif
#define SEARCH_STACK ((size_t)8 << 20)
/* Line numbers are cached per piece of this size. */
#ifndef LINE_PIECE
#define LINE_PIECE ((size_t)64 << 20)
#endif
size_t line_end(size_t p) {
    if (p >= size) return size;
    const unsigned char *q = memchr(data + p, '\n', size - p);
    return q ? (size_t)(q - data) : size;
}
size_t line_start(size_t p) {
    if (p > size) p = size;
#ifdef __linux__
    const unsigned char *q = memrchr(data, '\n', p);
    return q ? (size_t)(q - data) + 1 : 0;
#else
    while (p && data[p - 1] != '\n') --p;
    return p;
#endif
}
/* Counts documents: lines_reset starts a new one. A reloaded file can be
   mapped at its old address with its old size, so those cannot tell. */
static unsigned long document_generation;
/* Bounds of the line containing p. The last answer is kept so that repeated
   searches inside one very long line do not rescan it each time; it belongs
   to one document, which changes when files switch, one is reloaded, or a
   followed file grows. */
static void line_bounds(size_t p, size_t *begin, size_t *end) {
    static _Thread_local size_t b, e, cached_size;
    static _Thread_local const unsigned char *cached_data;
    static _Thread_local unsigned long cached_generation;
    if (cached_data != data || cached_size != size || cached_generation != document_generation ||
        p < b || p > e) {
        b = line_start(p); e = line_end(p);
        cached_data = data; cached_size = size; cached_generation = document_generation;
    }
    *begin = b; *end = e;
}
size_t next_line(size_t p) {
    size_t e = line_end(p);
    return e < size ? e + 1 : size;
}
size_t previous_line(size_t p) {
    return p ? line_start(p - 1) : 0;
}
void clear_search(Search *s) {
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
bool set_search(Search *s, Mode mode, const char *text, int case_mode, int subpattern,
                char *error, size_t cap) {
    Search n = {.mode = mode, .len = strlen(text), .subpattern = mode == REGEX ? subpattern : 0};
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
        if ((size_t)n.subpattern > n.re.re_nsub) {
            snprintf(error, cap, "No subpattern %d in the pattern", n.subpattern);
            regfree(&n.re);
            free(n.text);
            return false;
        }
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
            if (!s->subpattern) {
                regmatch_t match = {.rm_so = (regoff_t)(from - p), .rm_eo = (regoff_t)(end - p)};
                int rc = regexec(&s->re, (const char *)data + p, 1, &match,
                                 REG_STARTEND | (from > p ? REG_NOTBOL : 0));
                if (!rc) { *hit = p + (size_t)match.rm_so; return 1; }
                if (rc != REG_NOMATCH) { regerror(rc, &s->re, error, cap); return -1; }
            } else for (;;) {
                /* With ^S, the group must take part; otherwise try later starts. */
                regmatch_t match[10];
                match[0].rm_so = (regoff_t)(from - p);
                match[0].rm_eo = (regoff_t)(end - p);
                int rc = regexec(&s->re, (const char *)data + p, (size_t)s->subpattern + 1, match,
                                 REG_STARTEND | (from > p ? REG_NOTBOL : 0));
                if (rc == REG_NOMATCH) break;
                if (rc) { regerror(rc, &s->re, error, cap); return -1; }
                const regmatch_t *group = &match[s->subpattern];
                if (group->rm_so >= 0) { *hit = p + (size_t)group->rm_so; return 1; }
                from = p + (size_t)match[0].rm_so + 1;
                if (from > end) break;
            }
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
/* Searches pieces until none are left. glibc serializes regexec on one
   compiled pattern, so each started thread (`copy`) compiles its own; if
   that fails it shares the original, which the calling thread uses. */
static void search_run(Parallel *job, bool copy) {
    Search local = *job->s;
    bool own = copy && local.mode == REGEX && !local.regex_literal &&
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
}
static void *search_worker(void *arg) {
    search_run(arg, true);
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
    search_run(&job, false);
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    if (interrupted) return -1;
    if (job.rc == 1) *hit = job.hit;
    else if (job.rc < 0) snprintf(error, cap, "%s", job.error);
    return job.rc;
}
/* The part nearest the starting point is searched on this thread, so nearby
   matches cost no thread start-up; the rest is divided among all threads. */
int search_forward(const Search *s, size_t start, size_t limit,
                          size_t *hit, char *error, size_t cap) {
    if (start > limit || limit > size) return 0;
    if (search_threads < 2 || limit - start <= SEARCH_NEAR)
        return forward_scan(s, start, limit, hit, error, cap);
    int rc = forward_piece(s, start, start + SEARCH_NEAR, limit, true, hit, error, cap);
    return rc ? rc : search_pieces(s, true, start + SEARCH_NEAR, limit, limit, hit, error, cap);
}
int search_backward(const Search *s, size_t before, size_t *hit, char *error, size_t cap) {
    if (before > size) before = size;
    if (search_threads < 2 || before <= SEARCH_NEAR)
        return backward_scan(s, before, 0, hit, error, cap);
    int rc = backward_piece(s, before - SEARCH_NEAR, before, before, hit, error, cap);
    return rc ? rc : search_pieces(s, false, 0, before - SEARCH_NEAR, before, hit, error, cap);
}
int line_matches(const Search *s, size_t begin, size_t end) {
    size_t hit;
    char error[1];
    return search_forward(s, begin, end, &hit, error, sizeof(error));
}
/* Lines are tested one at a time, so a run of matching lines costs one short
   search each and a non-matching line is found as soon as it is reached. */
int nonmatch_forward(const Search *s, size_t from, size_t *hit, char *error, size_t cap) {
    for (size_t p = from; p < size;) {
        if (interrupted) return -1;
        size_t end = line_end(p);
        int rc = search_forward(s, p, end, hit, error, cap);
        if (rc < 0) return -1;
        if (!rc) { *hit = p; return 1; }
        if (end >= size) break;
        p = end + 1;
    }
    return 0;
}
int nonmatch_backward(const Search *s, size_t before, size_t *hit, char *error, size_t cap) {
    if (before > size) before = size;
    for (size_t p = before ? line_start(before - 1) : 0; before;) {
        if (interrupted) return -1;
        int rc = search_forward(s, p, line_end(p), hit, error, cap);
        if (rc < 0) return -1;
        if (!rc) { *hit = p; return 1; }
        if (!p) break;
        p = line_start(p - 1);
    }
    return 0;
}
/* Calls regexec for a match starting at or after `from` on the line
   [begin, end); 0 for a match, as regexec. */
static int regex_at(const Search *s, size_t begin, size_t end, size_t from, regmatch_t *match) {
    match[0].rm_so = (regoff_t)(from - begin);
    match[0].rm_eo = (regoff_t)(end - begin);
    return regexec(&s->re, (const char *)data + begin, (size_t)s->subpattern + 1, match,
                   REG_STARTEND | (from > begin ? REG_NOTBOL : 0));
}
size_t match_end(const Search *s, size_t hit) {
    if (s->mode == LITERAL) return s->len > size - hit ? size : hit + s->len;
    if (s->regex_literal) return hit + s->fast.len;
    size_t begin, end;
    line_bounds(hit, &begin, &end);
    if (s->mode == GLOB) return end;
    if (end - begin > INT_MAX) return hit;
    regmatch_t match[10];
    for (size_t from = begin; from <= end && !regex_at(s, begin, end, from, match);) {
        const regmatch_t *group = &match[s->subpattern];
        if (group->rm_so >= 0 && begin + (size_t)group->rm_so == hit) return begin + (size_t)group->rm_eo;
        if (begin + (size_t)match[0].rm_so > hit) break;
        from = begin + (size_t)match[0].rm_so + 1;
    }
    return hit;
}
/* One byte per marked input byte, independent of file/line size. Regexes
   still see the complete line so clipping does not change anchors or greediness. */
void search_marks(const Search *s, size_t begin, size_t end, size_t from, size_t to,
                  unsigned char *marks) {
    if (to <= from) return;
    memset(marks, 0, to - from);
    if (!s->text || interrupted) return;
    if (s->mode == GLOB) {
        if (glob_match(s, begin, end)) memset(marks, 1, to - from);
        return;
    }
    size_t cursor = begin, limit = end;
    if (s->mode == LITERAL) {
        /* Include matches crossing into/out of the range, even across newlines. */
        cursor = from >= s->len - 1 ? from - (s->len - 1) : 0;
        limit = s->len - 1 > size - to ? size : to + s->len - 1;
    } else if (s->regex_literal) {
        /* Only matches starting before the end of the range matter. */
        if (from - begin > s->fast.len - 1) cursor = from - (s->fast.len - 1);
        if (end - to > s->fast.len - 1) limit = to + s->fast.len - 1;
    } else if (end - begin > INT_MAX) return;
    while (cursor < to && !interrupted) {
        size_t a, b, next;
        if (s->mode == LITERAL) {
            char error[1];
            if (search_forward(s, cursor, limit, &a, error, sizeof(error)) != 1) break;
            b = a + s->len;
            next = a + 1;
        } else if (s->regex_literal) {
            const unsigned char *q = literal_find(&s->fast, data + cursor, limit - cursor, &interrupted);
            if (!q) break;
            a = (size_t)(q - data); b = a + s->fast.len;
            next = a + 1;
        } else {
            regmatch_t match[10];
            if (cursor > end || regex_at(s, begin, end, cursor, match)) break;
            const regmatch_t *group = &match[s->subpattern];
            next = begin + (size_t)match[0].rm_so + 1;
            if (group->rm_so < 0) { cursor = next; continue; }
            a = begin + (size_t)group->rm_so;
            b = begin + (size_t)group->rm_eo;
            if (begin + (size_t)match[0].rm_so >= to) break;
        }
        if (a >= to && s->mode != REGEX) break;
        size_t lo = a < from ? from : a;
        size_t hi = b > to ? to : b;
        if (hi > lo) memset(marks + lo - from, 1, hi - lo);
        /* Include overlaps; zero-length regex matches advance without painting. */
        cursor = next;
    }
}
/* Line numbers: newlines per complete LINE_PIECE (SIZE_MAX until counted),
   with running totals for the counted prefix, and anchors, recent positions
   whose numbers were found, so that nearby lines count only the bytes in
   between. There are several, as a prompt may show both the top line's
   number and the total, which are far apart. */
#ifndef LINE_SYNC_PIECES
#define LINE_SYNC_PIECES 2
#endif
#define ANCHOR_NEAR ((size_t)4 << 20)
#define ANCHORS 4
struct LineCache {
    size_t *pieces, *before; /* before[k]: newlines before piece k, for k <= prefix */
    size_t slots, prefix, document_size;
    struct { size_t pos, line; } anchors[ANCHORS];
    unsigned anchor_count, anchor_next;
};
static LineCache cache;
void lines_reset(void) {
    ++document_generation;
    free(cache.pieces);
    free(cache.before);
    cache = (LineCache){0};
}
LineCache *lines_detach(void) {
    LineCache *c = malloc(sizeof(*c));
    if (c) { *c = cache; c->document_size = size; cache = (LineCache){0}; }
    else lines_reset();
    return c;
}
void lines_attach(LineCache *c, bool appended) {
    lines_reset();
    if (!c) return;
    if (c->document_size == size || (appended && c->document_size < size)) cache = *c;
    else { free(c->pieces); free(c->before); }
    free(c);
}
void lines_free(LineCache *c) {
    if (c) { free(c->pieces); free(c->before); free(c); }
}
static bool pieces_ready(void) {
    size_t count = size / LINE_PIECE;
    if (count <= cache.slots && cache.before) return true;
    size_t *grown = realloc(cache.pieces, (count ? count : 1) * sizeof(*grown));
    if (!grown) return false;
    cache.pieces = grown;
    size_t *totals = realloc(cache.before, (count + 1) * sizeof(*totals));
    if (!totals) return false;
    if (!cache.before) totals[0] = 0;
    cache.before = totals;
    for (size_t i = cache.slots; i < count; ++i) cache.pieces[i] = SIZE_MAX;
    cache.slots = count;
    return true;
}
static void advance_prefix(void) {
    while (cache.prefix < cache.slots && cache.pieces[cache.prefix] != SIZE_MAX) {
        cache.before[cache.prefix + 1] = cache.before[cache.prefix] + cache.pieces[cache.prefix];
        ++cache.prefix;
    }
}
typedef struct { atomic_size_t next; size_t end; } Counting;
static void *count_worker(void *arg) {
    Counting *job = arg;
    for (;;) {
        size_t i = atomic_fetch_add(&job->next, 1);
        if (i >= job->end || interrupted) break;
        if (cache.pieces[i] == SIZE_MAX)
            cache.pieces[i] = literal_count(data + i * LINE_PIECE, LINE_PIECE, '\n');
    }
    return NULL;
}
/* Counts the pieces [from, to) on all search threads, then extends the
   counted prefix. */
static void count_pieces(size_t from, size_t to) {
    if (from >= to) return;
    Counting job = {.end = to};
    atomic_init(&job.next, from);
    pthread_t threads[256];
    unsigned started = 0;
    while (started + 1 < search_threads && started + 1 < to - from &&
           !pthread_create(&threads[started], NULL, count_worker, &job)) ++started;
    count_worker(&job);
    for (unsigned i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    advance_prefix();
}
/* Extends the prefix to piece k, in batches that double, so that a nearby
   target costs little and a distant one uses every thread. */
static bool count_to(size_t k) {
    advance_prefix();
    for (size_t batch = 1; cache.prefix < k;) {
        count_pieces(cache.prefix, cache.prefix + batch > k ? k : cache.prefix + batch);
        if (interrupted) return false;
        if (batch < 2 * (size_t)search_threads) batch *= 2;
    }
    return true;
}
/* The anchor nearest pos, or -1 when there is none; *distance is how far. */
static int nearest_anchor(size_t pos, size_t *distance) {
    int best = -1;
    for (unsigned i = 0; i < cache.anchor_count; ++i) {
        size_t a = cache.anchors[i].pos, d = pos > a ? pos - a : a - pos;
        if (a <= size && (best < 0 || d < *distance)) { best = (int)i; *distance = d; }
    }
    return best;
}
/* Moves a nearby anchor to pos, or replaces the oldest one. */
static void anchor(size_t pos, size_t number) {
    size_t d;
    int i = nearest_anchor(pos, &d);
    if (i < 0 || d > ANCHOR_NEAR) {
        if (cache.anchor_count < ANCHORS) i = (int)cache.anchor_count++;
        else { i = (int)cache.anchor_next; cache.anchor_next = (cache.anchor_next + 1) % ANCHORS; }
    }
    cache.anchors[i].pos = pos;
    cache.anchors[i].line = number;
}
int line_number_known(size_t pos, size_t *number) {
    if (pos > size) pos = size;
    size_t d;
    int i = nearest_anchor(pos, &d);
    if (i >= 0 && (d <= ANCHOR_NEAR || d <= pos % LINE_PIECE)) {
        size_t a = cache.anchors[i].pos, line = cache.anchors[i].line;
        *number = pos >= a ? line + literal_count(data + a, pos - a, '\n')
                           : line - literal_count(data + pos, a - pos, '\n');
        anchor(pos, *number);
        return 1;
    }
    if (!pieces_ready()) return -1;
    size_t k = pos / LINE_PIECE;
    advance_prefix();
    if (cache.prefix < k) {
        /* A few missing pieces are cheap enough to count now. */
        if (k - cache.prefix > LINE_SYNC_PIECES) return 0;
        count_pieces(cache.prefix, k);
        if (cache.prefix < k) return -1;
    }
    *number = cache.before[k] + literal_count(data + k * LINE_PIECE, pos - k * LINE_PIECE, '\n') + 1;
    anchor(pos, *number);
    return 1;
}
bool lines_count_more(size_t pos, size_t bytes) {
    size_t pieces = bytes / LINE_PIECE ? bytes / LINE_PIECE : 1;
    if (pos > size) pos = size;
    if (!pieces_ready()) return false;
    advance_prefix();
    size_t k = pos / LINE_PIECE;
    if (cache.prefix >= k) return false;
    count_pieces(cache.prefix, cache.prefix + pieces > k ? k : cache.prefix + pieces);
    return cache.prefix < k && !interrupted;
}
double lines_progress(size_t pos) {
    size_t k = (pos > size ? size : pos) / LINE_PIECE;
    return k ? (double)(cache.prefix < k ? cache.prefix : k) / (double)k : 1.0;
}
int line_number(size_t pos, size_t *number) {
    if (pos > size) pos = size;
    if (!pieces_ready()) return -1;
    if (!count_to(pos / LINE_PIECE)) return -1;
    return line_number_known(pos, number);
}
int line_position(size_t number, size_t *pos) {
    *pos = 0;
    if (number <= 1) return 1;
    if (!pieces_ready()) return -1;
    size_t want = number - 1;
    advance_prefix();
    for (size_t batch = 1; cache.before[cache.prefix] < want && cache.prefix < cache.slots;) {
        size_t to = cache.prefix + batch > cache.slots ? cache.slots : cache.prefix + batch;
        count_pieces(cache.prefix, to);
        if (interrupted) return -1;
        if (batch < 2 * (size_t)search_threads) batch *= 2;
    }
    /* The last piece starting with fewer than `want` newlines before it. */
    size_t lo = 0, hi = cache.prefix;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (cache.before[mid] < want) lo = mid; else hi = mid - 1;
    }
    size_t seen = cache.before[lo], p = lo * LINE_PIECE;
    for (size_t ticks = 0; seen < want; ++ticks) {
        if (!(ticks & 4095) && interrupted) return -1;
        const unsigned char *q = p < size ? memchr(data + p, '\n', size - p) : NULL;
        if (!q) break;
        p = (size_t)(q - data) + 1;
        ++seen;
    }
    if (seen < want || p >= size) {
        /* Beyond the end: the last line. */
        *pos = size ? line_start(size - (data[size - 1] == '\n')) : 0;
        return 0;
    }
    *pos = p;
    anchor(p, number);
    return 1;
}
