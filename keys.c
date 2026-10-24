#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "keys.h"
#include "search.h"
#include <sys/select.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int tty_fd = 0;
volatile sig_atomic_t resized, exiting, suspended;

static void signal_handler(int sig) {
    if (sig == SIGWINCH) resized = 1;
    else if (sig == SIGTSTP) suspended = 1;
    else { interrupted = 1; if (sig == SIGTERM || sig == SIGHUP) exiting = sig; }
}
void signals_install(void) {
    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) sigaction(signals[i], &sa, NULL);
}

/* Input queued by +commands, -p, and lesskey extra strings. */
static unsigned char *queue;
static size_t queue_len, queue_cap, queue_pos;
void keys_queue(const char *text, size_t len, bool front) {
    if (queue_pos) {
        memmove(queue, queue + queue_pos, queue_len - queue_pos);
        queue_len -= queue_pos;
        queue_pos = 0;
    }
    if (queue_len + len > queue_cap) {
        size_t cap = (queue_len + len) * 2 + 64;
        unsigned char *grown = realloc(queue, cap);
        if (!grown) return;
        queue = grown;
        queue_cap = cap;
    }
    if (front) {
        memmove(queue + len, queue, queue_len);
        memcpy(queue, text, len);
    } else memcpy(queue + queue_len, text, len);
    queue_len += len;
}
void keys_discard(void) { queue_len = queue_pos = 0; }
bool keys_queued(void) { return queue_pos < queue_len; }
bool keys_waiting(void) {
    fd_set in;
    FD_ZERO(&in);
    FD_SET(tty_fd, &in);
    struct timeval none = {0, 0};
    return select(tty_fd + 1, &in, NULL, NULL, &none) > 0;
}

int wake_fd = -1;
static int next_key(int timeout_ms, bool wake) {
    if (queue_pos < queue_len) return queue[queue_pos++];
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT); sigaddset(&block, SIGTERM); sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGWINCH); sigaddset(&block, SIGTSTP);
    for (;;) {
        /* Signals are held off between testing the flags and waiting, and let
           in only by pselect, so one that arrived earlier (say during a
           redraw) cannot be left unnoticed until the next keypress. */
        sigprocmask(SIG_BLOCK, &block, &old);
        bool pending = interrupted || resized || exiting || suspended;
        int ready = 0, saved_errno = 0;
        if (!pending) {
            fd_set in;
            FD_ZERO(&in); FD_SET(tty_fd, &in);
            int top = tty_fd;
            if (wake && wake_fd >= 0) { FD_SET(wake_fd, &in); if (wake_fd > top) top = wake_fd; }
            struct timespec wait = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L};
            ready = pselect(top + 1, &in, NULL, NULL, timeout_ms < 0 ? NULL : &wait, &old);
            saved_errno = errno;
            if (ready > 0 && !FD_ISSET(tty_fd, &in)) ready = -2;
        }
        sigprocmask(SIG_SETMASK, &old, NULL);
        if (pending) return KEY_SIGNAL;
        if (ready == -2) return KEY_WAKE;
        if (ready < 0) { if (saved_errno != EINTR) return KEY_EOF; continue; }
        if (!ready) return KEY_TIMEOUT;
        unsigned char c;
        ssize_t n = read(tty_fd, &c, 1);
        if (n == 1) return c;
        if (!n || (n < 0 && errno != EINTR && errno != EAGAIN)) return KEY_EOF;
    }
}
int key_read(int timeout_ms) { return next_key(timeout_ms, false); }
int key_wait(int timeout_ms) { return next_key(timeout_ms, true); }

typedef struct {
    unsigned char keys[32];
    size_t len;
    int action;
    char *extra;
} Binding;
typedef struct {
    Binding *items;
    size_t count, cap;
} Table;
/* User bindings come first and win; #stop drops the defaults behind them. */
static Table user_commands, default_commands, user_edits, default_edits;
static bool stop_commands, stop_edits;
enum { FOUND, PREFIX, NONE };
static int lookup_in(const Table *t, const unsigned char *buf, size_t n, const Binding **found,
                     bool *prefix) {
    for (size_t i = 0; i < t->count; ++i) {
        const Binding *b = &t->items[i];
        if (b->len < n || memcmp(b->keys, buf, n)) continue;
        if (b->len == n) { *found = b; return FOUND; }
        *prefix = true;
    }
    return NONE;
}
static int lookup(bool edit, const unsigned char *buf, size_t n, const Binding **found) {
    bool prefix = false;
    if (lookup_in(edit ? &user_edits : &user_commands, buf, n, found, &prefix) == FOUND) return FOUND;
    if (!(edit ? stop_edits : stop_commands) &&
        lookup_in(edit ? &default_edits : &default_commands, buf, n, found, &prefix) == FOUND)
        return FOUND;
    return prefix ? PREFIX : NONE;
}
/* Reads the rest of an unrecognized ESC [ or ESC O sequence so that its
   bytes are not taken as commands. */
static void swallow(const unsigned char *buf, size_t n) {
    if (n < 2 || buf[0] != 0x1B || (buf[1] != '[' && buf[1] != 'O')) return;
    if (buf[1] == 'O') { if (n == 2) key_read(100); return; }
    int c = n > 2 ? buf[n - 1] : 0;
    for (int count = 0; (n == 2 || c < 0x40 || c > 0x7E) && count < 64; ++count) {
        c = key_read(100);
        if (c < 0) return;
        n = 3;
    }
}
Action read_command(char *extra, size_t cap) {
    unsigned char buf[32];
    size_t n = 0;
    if (cap) *extra = 0;
    for (;;) {
        int c = key_read(-1);
        if (c == KEY_SIGNAL) return A_NOACTION;
        if (c < 0) return A_QUIT;
        buf[n++] = (unsigned char)c;
        const Binding *b;
        int r = lookup(false, buf, n, &b);
        if (r == FOUND) {
            if (cap) snprintf(extra, cap, "%s", b->extra ? b->extra : "");
            return (Action)b->action;
        }
        if (r == PREFIX && n < sizeof(buf)) continue;
        swallow(buf, n);
        return A_INVALID;
    }
}
EditAction read_edit(int *first) {
    unsigned char buf[32];
    size_t n = 0;
    *first = 0;
    for (;;) {
        /* A lone ESC, with nothing following promptly, cancels the prompt. */
        int c = key_read(n && buf[0] == 0x1B ? 300 : -1);
        if (c == KEY_TIMEOUT) return E_ABORT;
        if (c < 0) { *first = c; return E_ABORT; }
        if (!n) *first = c;
        buf[n++] = (unsigned char)c;
        const Binding *b;
        int r = lookup(true, buf, n, &b);
        if (r == FOUND) return (EditAction)b->action;
        if (r == PREFIX && n < sizeof(buf)) continue;
        if (n == 1) return E_INSERT;
        swallow(buf, n);
        return E_IGNORE;
    }
}

static const struct { const char *name; int action; } command_names[] = {
    {"back-bracket", A_B_BRACKET}, {"back-line", A_BACK_LINE}, {"back-line-force", A_BACK_LINE_FORCE},
    {"back-screen", A_BACK_SCREEN}, {"back-scroll", A_BACK_SCROLL}, {"back-search", A_B_SEARCH},
    {"back-window", A_BACK_WINDOW}, {"clear-mark", A_CLRMARK}, {"clear-search", A_CLEAR_SEARCH},
    {"debug", A_NOACTION}, {"digit", A_DIGIT}, {"display-flag", A_DISP_OPTION},
    {"display-option", A_DISP_OPTION}, {"end", A_GOEND}, {"end-scroll", A_RIGHT_END},
    {"examine", A_EXAMINE}, {"filter", A_FILTER}, {"first-cmd", A_FIRSTCMD}, {"firstcmd", A_FIRSTCMD},
    {"flush-repaint", A_FLUSH_REPAINT}, {"forw-bracket", A_F_BRACKET}, {"forw-forever", A_FORW_FOREVER},
    {"forw-line", A_FORW_LINE}, {"forw-line-force", A_FORW_LINE_FORCE}, {"forw-screen", A_FORW_SCREEN},
    {"forw-screen-force", A_FORW_SCREEN_FORCE}, {"forw-scroll", A_FORW_SCROLL},
    {"forw-search", A_F_SEARCH}, {"forw-until-hilite", A_FORW_UNTIL_HILITE},
    {"forw-window", A_FORW_WINDOW}, {"goto-end", A_GOEND}, {"goto-end-buffered", A_GOEND},
    {"goto-line", A_GOLINE}, {"goto-mark", A_GOMARK}, {"goto-pos", A_GOPOS}, {"help", A_HELP},
    {"index-file", A_INDEX_FILE}, {"invalid", A_INVALID}, {"left-scroll", A_LEFT_SCROLL},
    {"mouse", A_X11MOUSE}, {"mouse6", A_X116MOUSE}, {"next-file", A_NEXT_FILE},
    {"next-tag", A_INVALID}, {"no-scroll", A_LEFT_END}, {"noaction", A_NOACTION},
    {"osc8-back-search", A_INVALID}, {"osc8-forw-search", A_INVALID}, {"osc8-jump", A_INVALID},
    {"osc8-open", A_INVALID}, {"percent", A_PERCENT}, {"pipe", A_PIPE}, {"prev-file", A_PREV_FILE},
    {"prev-tag", A_INVALID}, {"pshell", A_PSHELL}, {"quit", A_QUIT}, {"remove-file", A_REMOVE_FILE},
    {"repaint", A_REPAINT}, {"repaint-flush", A_FLUSH_REPAINT}, {"repeat-search", A_AGAIN_SEARCH},
    {"repeat-search-all", A_T_AGAIN_SEARCH}, {"reverse-search", A_REVERSE_SEARCH},
    {"reverse-search-all", A_T_REVERSE_SEARCH}, {"right-scroll", A_RIGHT_SCROLL},
    {"save-file", A_SAVE}, {"set-mark", A_SETMARK}, {"set-mark-bottom", A_SETMARKBOT},
    {"shell", A_SHELL}, {"status", A_STAT}, {"toggle-flag", A_OPT_TOGGLE},
    {"toggle-option", A_OPT_TOGGLE}, {"undo-hilite", A_UNDO_HILITE}, {"version", A_VERSION},
    {"visual", A_VISUAL}, {"regex-mode", A_REGEX_MODE}, {"literal-mode", A_LITERAL_MODE},
    {"glob-mode", A_GLOB_MODE}};
static const struct { const char *name; int action; } edit_names[] = {
    {"abort", E_ABORT}, {"back-complete", E_IGNORE}, {"backspace", E_BACKSPACE},
    {"delete", E_DELETE}, {"down", E_DOWN}, {"end", E_END}, {"expand", E_IGNORE},
    {"forw-complete", E_IGNORE}, {"home", E_HOME}, {"insert", E_IGNORE}, {"kill-line", E_KILL_LINE},
    {"left", E_LEFT}, {"literal", E_LITERAL}, {"right", E_RIGHT}, {"up", E_UP},
    {"word-backspace", E_WORD_BACKSPACE}, {"word-delete", E_WORD_DELETE},
    {"word-left", E_WORD_LEFT}, {"word-right", E_WORD_RIGHT}};

/* Terminal sequences for lesskey's \k key names; there is no termcap, so
   the common xterm and VT forms are all bound. */
static const char *key_name(char k) {
    switch (k) {
    case 'b': return "\x7f";
    case 'B': return "\b";
    case 'd': return "\033[B|\033OB";
    case 'D': return "\033[6~";
    case 'e': return "\033[F|\033OF|\033[4~|\033[8~";
    case 'h': return "\033[H|\033OH|\033[1~|\033[7~";
    case 'i': return "\033[2~";
    case 'l': return "\033[D|\033OD";
    case 'L': return "\033[1;5D";
    case 'r': return "\033[C|\033OC";
    case 'R': return "\033[1;5C";
    case 't': return "\033[Z";
    case 'u': return "\033[A|\033OA";
    case 'U': return "\033[5~";
    case 'x': return "\033[3~";
    case 'X': return "\033[3;5~";
    case '1': return "\033OP|\033[11~";
    default: return NULL;
    }
}
typedef struct {
    unsigned char text[16][32];
    size_t len[16], count;
} Variants;
static bool append(Variants *v, const char *alternatives, bool split) {
    /* Each existing variant is extended by every '|'-separated alternative
       of a key name, or by one literal character. */
    Variants out = {0};
    for (size_t i = 0; i < v->count; ++i) {
        for (const char *a = alternatives;;) {
            const char *bar = split ? strchr(a, '|') : NULL;
            size_t n = bar ? (size_t)(bar - a) : strlen(a);
            if (out.count == 16 || v->len[i] + n > sizeof(v->text[0])) return false;
            memcpy(out.text[out.count], v->text[i], v->len[i]);
            memcpy(out.text[out.count] + v->len[i], a, n);
            out.len[out.count++] = v->len[i] + n;
            if (!bar) break;
            a = bar + 1;
        }
    }
    *v = out;
    return true;
}
/* One escaped character of a lesskey string, written to out; returns false
   for a malformed escape. */
static bool unescape(const char **p, char out[2], const char **keys) {
    const char *s = *p;
    *keys = NULL;
    out[1] = 0;
    if (*s == '^' && s[1]) {
        out[0] = s[1] == '?' ? 0x7f : (char)(s[1] & 0x1f);
        *p = s + 2;
        return true;
    }
    if (*s != '\\' || !s[1]) { out[0] = *s; *p = s + 1; return true; }
    char c = s[1];
    *p = s + 2;
    switch (c) {
    case 'e': out[0] = 0x1b; return true;
    case 'b': out[0] = '\b'; return true;
    case 'n': out[0] = '\n'; return true;
    case 'r': out[0] = '\r'; return true;
    case 't': out[0] = '\t'; return true;
    case 'k': *keys = key_name(s[2]); if (s[2]) *p = s + 3; return *keys != NULL;
    default:
        if (c >= '0' && c <= '7') {
            int v = 0, i = 1;
            while (i <= 3 && s[i] >= '0' && s[i] <= '7') v = v * 8 + s[i++] - '0';
            out[0] = (char)v;
            *p = s + i;
            return true;
        }
        out[0] = c;
        return true;
    }
}
static bool add(Table *t, const unsigned char *keys, size_t len, int action, const char *extra) {
    if (t->count == t->cap) {
        size_t cap = t->cap * 2 + 64;
        Binding *grown = realloc(t->items, cap * sizeof(*grown));
        if (!grown) return false;
        t->items = grown;
        t->cap = cap;
    }
    Binding *b = &t->items[t->count++];
    memcpy(b->keys, keys, len);
    b->len = len;
    b->action = action;
    b->extra = extra && *extra ? strdup(extra) : NULL;
    return true;
}
static bool is_space(char c) { return c == ' ' || c == '\t'; }
static bool parse(const char *text, const char *where, bool defaults, char *error, size_t cap) {
    enum { COMMAND, EDIT, ENV } section = COMMAND;
    bool ok = true;
    int number = 0;
    for (const char *line = text; *line;) {
        const char *eol = strchr(line, '\n');
        size_t n = eol ? (size_t)(eol - line) : strlen(line);
        char buf[1024];
        snprintf(buf, sizeof(buf), "%.*s", (int)(n < sizeof(buf) ? n : sizeof(buf) - 1), line);
        line += n + (eol != NULL);
        ++number;
        char *s = buf;
        while (is_space(*s)) ++s;
        size_t end = strlen(s);
        while (end && (is_space(s[end - 1]) || s[end - 1] == '\r')) s[--end] = 0;
        if (!*s) continue;
        if (*s == '#') {
            if (!strcmp(s, "#command")) section = COMMAND;
            else if (!strcmp(s, "#line-edit")) section = EDIT;
            else if (!strcmp(s, "#env")) section = ENV;
            else if (!strcmp(s, "#stop")) {
                if (section == COMMAND) stop_commands = true;
                else if (section == EDIT) stop_edits = true;
            }
            continue;
        }
        if (section == ENV) {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            char *name_end = eq;
            bool plus = eq > s && eq[-1] == '+';
            if (plus) --name_end;
            while (name_end > s && is_space(name_end[-1])) --name_end;
            *name_end = 0;
            char *value = eq + 1;
            while (is_space(*value)) ++value;
            if (plus && getenv(s)) {
                char joined[2048];
                snprintf(joined, sizeof(joined), "%s%s", getenv(s), value);
                setenv(s, joined, 1);
            } else setenv(s, value, 1);
            continue;
        }
        Variants v = {.count = 1};
        const char *p = s;
        bool bad = false;
        while (*p && !is_space(*p)) {
            char one[2];
            const char *keys;
            if (!unescape(&p, one, &keys) || (!keys && !one[0]) || !append(&v, keys ? keys : one, keys != NULL)) {
                bad = true;
                break;
            }
        }
        while (is_space(*p)) ++p;
        const char *name = p;
        while (*p && !is_space(*p)) ++p;
        size_t name_len = (size_t)(p - name);
        while (is_space(*p)) ++p;
        char extra[256];
        size_t e = 0;
        while (*p && e + 1 < sizeof(extra)) {
            char one[2];
            const char *keys;
            if (!unescape(&p, one, &keys) || keys) { bad = true; break; }
            extra[e++] = one[0];
        }
        extra[e] = 0;
        int action = -1;
        if (section == COMMAND) {
            for (size_t i = 0; i < sizeof(command_names) / sizeof(command_names[0]); ++i)
                if (strlen(command_names[i].name) == name_len && !memcmp(command_names[i].name, name, name_len))
                    action = command_names[i].action;
        } else {
            for (size_t i = 0; i < sizeof(edit_names) / sizeof(edit_names[0]); ++i)
                if (strlen(edit_names[i].name) == name_len && !memcmp(edit_names[i].name, name, name_len))
                    action = edit_names[i].action;
        }
        if (bad || action < 0) {
            if (ok) snprintf(error, cap, "%s:%d: %s", where, number,
                             bad ? "invalid key string" : "unknown action");
            ok = false;
            continue;
        }
        Table *t = section == COMMAND ? (defaults ? &default_commands : &user_commands)
                                      : (defaults ? &default_edits : &user_edits);
        for (size_t i = 0; i < v.count; ++i) add(t, v.text[i], v.len[i], action, extra);
    }
    return ok;
}
bool lesskey_parse(const char *text, const char *where, char *error, size_t cap) {
    return parse(text, where, false, error, cap);
}
bool lesskey_file(const char *path, bool required, char *error, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (required) snprintf(error, cap, "%s: cannot open lesskey file", path);
        return !required;
    }
    char *text = NULL;
    size_t len = 0, room = 0;
    for (;;) {
        if (len + 4096 + 1 > room) {
            room = (len + 4096 + 1) * 2;
            char *grown = realloc(text, room);
            if (!grown) break;
            text = grown;
        }
        size_t n = fread(text + len, 1, 4096, f);
        len += n;
        if (n < 4096) break;
    }
    fclose(f);
    if (!text) return false;
    text[len] = 0;
    bool ok = parse(text, path, false, error, cap);
    free(text);
    return ok;
}
/* The default bindings, in lesskey source form. */
static const char defaults[] =
    "#command\n"
    "\\r forw-line\n\\n forw-line\ne forw-line\nj forw-line\n\\kd forw-line\n^E forw-line\n"
    "^N forw-line\n\\e[1;5B forw-line\n\\e[1;2B forw-line\n"
    "y back-line\n^Y back-line\nk back-line\n^K back-line\n^P back-line\n\\ku back-line\n"
    "\\e[1;5A back-line\n\\e[1;2A back-line\n"
    "J forw-line-force\nK back-line-force\nY back-line-force\n"
    "\\040 forw-screen\nf forw-screen\n^F forw-screen\n^V forw-screen\n\\kD forw-screen\n"
    "\\e\\040 forw-screen-force\n"
    "b back-screen\n^B back-screen\n\\ev back-screen\n\\kU back-screen\n"
    "z forw-window\nw back-window\n"
    "d forw-scroll\n^D forw-scroll\nu back-scroll\n^U back-scroll\n"
    "\\e) right-scroll\n\\kr right-scroll\n\\e( left-scroll\n\\kl left-scroll\n"
    "\\e} end-scroll\n\\kR end-scroll\n\\e{ no-scroll\n\\kL no-scroll\n"
    "F forw-forever\n\\eF forw-until-hilite\nR repaint-flush\nr repaint\n^R repaint\n^L repaint\n"
    "\\eu undo-hilite\n\\eU clear-search\n"
    "g goto-line\n\\kh goto-line\n< goto-line\n\\e< goto-line\n"
    "p percent\n% percent\nP goto-pos\n"
    "{ forw-bracket {}\n( forw-bracket ()\n[ forw-bracket []\n"
    "} back-bracket {}\n) back-bracket ()\n] back-bracket []\n"
    "\\e^F forw-bracket\n\\e^B back-bracket\n"
    "G goto-end\n\\e> goto-end\n> goto-end\n\\ke goto-end\n"
    "= status\n^G status\n:f status\n"
    "/ forw-search\n? back-search\n"
    "n repeat-search\n\\en repeat-search-all\nN reverse-search\n\\eN reverse-search-all\n"
    "& filter\n"
    "m set-mark\nM set-mark-bottom\n\\em clear-mark\n' goto-mark\n^X^X goto-mark\n"
    ":e examine\n^X^V examine\n:n next-file\n:p prev-file\n:x index-file\n:d remove-file\n"
    "- toggle-option\n_ display-option\n| pipe\nv visual\n! shell\n\\# pshell\n+ firstcmd\n"
    "s save-file\nH help\nh help\nV version\n"
    "0 digit\n1 digit\n2 digit\n3 digit\n4 digit\n5 digit\n6 digit\n7 digit\n8 digit\n9 digit\n"
    "q quit\nQ quit\n:q quit\n:Q quit\nZZ quit\n"
    "\\e[M mouse\n\\e[< mouse6\n"
    "\\er regex-mode\n\\el literal-mode\n\\ew glob-mode\n"
    "#line-edit\n"
    "\\t forw-complete\n\\kt back-complete\n\\e\\t back-complete\n^L literal\n"
    "^V literal\n^A literal\n"
    "\\el right\n\\kr right\n\\eh left\n\\kl left\n"
    "\\eb word-left\n\\e\\kl word-left\n\\kL word-left\n"
    "\\ew word-right\n\\e\\kr word-right\n\\kR word-right\n\\ei insert\n"
    "\\ex delete\n\\kx delete\n\\eX word-delete\n\\kX word-delete\n\\e\\kx word-delete\n"
    "\\e\\b word-backspace\n\\e\\177 word-backspace\n"
    "\\e0 home\n\\kh home\n\\e$ end\n\\ke end\n\\ek up\n\\ku up\n\\ej down\n\\kd down\n"
    "^G abort\n\\177 backspace\n\\b backspace\n^U kill-line\n";
void keys_defaults(void) {
    char error[128];
    parse(defaults, "defaults", true, error, sizeof(error));
}
