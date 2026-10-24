#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "options.h"
#include "pager.h"
#include "keys.h"
#include "render.h"
#include "search.h"
#include "literal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void usage(FILE *out) {
    fputs("Usage: fastless [options] [file...]\n"
          "       fastless [options] --find PATTERN [--all] [--offset BYTE] [file]\n"
          "\n"
          "A pager for very large files with less's keys and options. Press h in the\n"
          "pager for a summary of commands. Plain files are mapped, never copied;\n"
          "gzip, zstd, xz, and bzip2 are detected by magic bytes and decoded first.\n"
          "Without a file, or with -, standard input is read. When output is not a\n"
          "terminal, the input is copied to it, as with cat.\n"
          "\n"
          "Searching:\n"
          "  --regex, --literal, --glob   POSIX extended regex (default), literal bytes,\n"
          "                               or whole-line glob (* ? \\); ESC-r/l/w switch\n"
          "  -i, -I, --case-sensitive     smart / always ignore ASCII case, or not\n"
          "  -a, -A                       start searches after the screen / target line\n"
          "  -g, -G                       highlight only the last match / nothing\n"
          "  --incsearch                  search while the pattern is typed\n"
          "  --search-options=[EFKNRW-]   default search modifiers\n"
          "  --find PATTERN [--all]       print matching byte offsets and exit\n"
          "  --offset BYTE                start (or search) at this byte offset\n"
          "Display:\n"
          "  -R, -r                       pass ANSI color sequences through\n"
          "  -N, -n                       show line numbers / none, even in prompts\n"
          "  --line-num-width=N           width of the -N column (default 7)\n"
          "  -s                           squeeze runs of blank lines\n"
          "  -x N[,N...]                  tab stops       -~  no tildes after the end\n"
          "  -# N                         horizontal scroll step (0: half a screen)\n"
          "  --header=L[,C[,N]]           keep L lines (from line N) and C columns fixed\n"
          "  -J, --status-col-width=N     show marks and matching lines in a status column\n"
          "  -u, -U, --proc-*/--PROC-*    how backspaces, returns, and tabs are shown\n"
          "  --rscroll=C                  character marking chopped lines ('-' for none)\n"
          "  -w, -W                       highlight the first new line after moving\n"
          "  -m, -M, -P PROMPT            less-style prompts\n"
          "  -c, -X, -F                   clear to repaint; no alternate screen; quit if\n"
          "                               the file fits on one screen\n"
          "  -e, -E, -K, -q, -Q           quit at end of file / on Ctrl-C; quiet bell\n"
          "  -z N, -j N                   window size; target line of jumps and searches\n"
          "  --mouse, --wheel-lines=N     mouse wheel scrolling\n"
          "  --redraw-on-quit             leave the final screen on the terminal\n"
          "Input:\n"
          "  -f                           read devices and other non-regular files\n"
          "  -L                           ignore LESSOPEN\n"
          "  --compression FORMAT         auto, none, gzip, zstd, xz, or bzip2\n"
          "  --max-mem GiB                decoded or piped input kept in memory; larger\n"
          "                               input goes to a temporary file (default: 10),\n"
          "                               and pipes beyond it are read as needed\n"
          "  --tmpdir DIR                 temporary storage (default: TMPDIR or /tmp)\n"
          "  --threads N                  search and xz threads (0: auto, at most 32)\n"
          "  --simd auto|off|avx2         runtime search acceleration\n"
          "  --lesskey-src=FILE           key bindings in lesskey source form\n"
          "  +CMD, ++CMD, -p PATTERN      commands to run when a file is opened\n"
          "\n"
          "LESS supplies default options, as for less; command-line options override\n"
          "them. See fastless(1) for the full documentation.\n", out);
}
static bool utf8_charset(void) {
    const char *charset = getenv("LESSCHARSET");
    if (charset && *charset) return !strcasecmp(charset, "utf-8") || !strcasecmp(charset, "utf8");
    const char *vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (size_t i = 0; i < 3; ++i) {
        const char *v = getenv(vars[i]);
        if (v && *v) return strcasestr(v, "utf-8") || strcasestr(v, "utf8");
    }
    return false;
}
/* lesskey source given as text, as by --lesskey-content and
   LESSKEY_CONTENT: lines end at newlines or semicolons, and \; is a
   semicolon. */
static void lesskey_content(const char *text, const char *where, char *error, size_t cap) {
    char *lines = malloc(strlen(text) + 1), *o = lines;
    if (!lines) return;
    for (const char *p = text; *p; ++p) {
        if (*p == '\\' && p[1] == ';') *o++ = *++p;
        else *o++ = *p == ';' ? '\n' : *p;
    }
    *o = 0;
    lesskey_parse(lines, where, error, cap);
    free(lines);
}
/* lesskey source files, which may also set environment variables such as
   LESS, are read before options. */
static void load_lesskey(int argc, char **argv) {
    const char *src = NULL, *content = NULL;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) break;
        if (!strncmp(a, "--lesskey-src=", 14)) src = a + 14;
        else if (!strcmp(a, "--lesskey-src") && i + 1 < argc) src = argv[++i];
        else if (!strncmp(a, "--lesskey-content=", 18)) content = a + 18;
        else if (!strcmp(a, "--lesskey-content") && i + 1 < argc) content = argv[++i];
    }
    char error[512] = "";
    if (src) lesskey_file(src, true, error, sizeof(error));
    else {
        char paths[3][4096];
        const char *candidates[4] = {getenv("LESSKEYIN"), NULL, NULL, NULL};
        const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && *xdg) { snprintf(paths[0], sizeof(paths[0]), "%s/lesskey", xdg); candidates[1] = paths[0]; }
        if (home && *home) {
            snprintf(paths[1], sizeof(paths[1]), "%s/.config/lesskey", home);
            snprintf(paths[2], sizeof(paths[2]), "%s/.lesskey", home);
            candidates[2] = paths[1];
            candidates[3] = paths[2];
        }
        for (int i = 0; i < 4; ++i)
            if (candidates[i] && *candidates[i] && !access(candidates[i], R_OK)) {
                lesskey_file(candidates[i], false, error, sizeof(error));
                break;
            }
    }
    const char *env = getenv("LESSKEY_CONTENT");
    if (env && *env) lesskey_content(env, "LESSKEY_CONTENT", error, sizeof(error));
    if (content) lesskey_content(content, "--lesskey-content", error, sizeof(error));
    if (error[0]) fprintf(stderr, "lesskey: %s\n", error);
}
int main(int argc, char **argv) {
    if (sizeof(void *) < 8) { fputs("A 64-bit build is required\n", stderr); return 2; }
    options_init();
    keys_defaults();
    render_setup();
    load_lesskey(argc, argv);
    const char *less = getenv("LESS");
    if (less) options_parse_env(less);
    char **names = calloc((size_t)argc + 1, sizeof(char *));
    if (!names) return 2;
    int count = 0;
    char error[512] = "";
    int rc = options_parse_argv(argc, argv, names, &count, error, sizeof(error));
    if (rc == -1) { fprintf(stderr, "%s\n", error); return 2; }
    if (rc == -3) { usage(stdout); return 0; }
    if (rc == -4) { puts(FASTLESS_VERSION); return 0; }
    if (opt.all && !opt.find) { fputs("--all requires --find\n", stderr); return 2; }
    if (!literal_simd_configure(opt.simd)) {
        fputs("Invalid SIMD mode or AVX2 unavailable; use auto, off, or avx2\n", stderr);
        return 2;
    }
    if (!opt.threads) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        opt.threads = cpus > 0 ? (unsigned)(cpus > 32 ? 32 : cpus) : 1;
    }
    search_threads = opt.threads;
    render.utf8 = utf8_charset();
    signals_install();
    bool stdin_tty = isatty(STDIN_FILENO);
    if (!count) {
        if (stdin_tty) { fputs("Missing filename (\"fastless --help\" for help)\n", stderr); return 2; }
        names[count++] = "-";
    }
    /* Without a terminal for output, fastless copies like cat, and copies
       standard input as it reads it rather than keeping it. */
    bool cat = !opt.find && !isatty(STDOUT_FILENO);
    for (int i = 0; i < count; ++i)
        if (!strcmp(names[i], "-")) {
            if (stdin_tty) { fputs("Standard input is a terminal\n", stderr); return 2; }
            if (!cat && (rc = pager_load_stdin())) return rc;
            break;
        }
    if (opt.find) return pager_find(names, count);
    if (cat) return pager_cat(names, count);
    if (!stdin_tty) {
        tty_fd = open("/dev/tty", O_RDONLY);
        if (tty_fd < 0) { fputs("Pager needs a terminal; use --find for noninteractive searching\n", stderr); return 2; }
    }
    return pager_run(names, count);
}
