#ifndef FASTLESS_RENDER_H
#define FASTLESS_RENDER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "search.h"

/* How file bytes become screen cells. Nothing from the file reaches the
   terminal except printable text and, with raw_color, SGR color sequences. */
typedef struct {
    bool utf8;          /* decode UTF-8; otherwise bytes >= 0x80 show as <XX> */
    bool raw_color;     /* -R: pass ESC [ ... m sequences through */
    bool ctl_backspace; /* show backspaces as ^H instead of overstriking */
    bool ctl_return;    /* show a CR before a newline as ^M instead of hiding it */
    bool ctl_tab;       /* show tabs as ^I instead of expanding them */
    int tabs[32], tab_count; /* explicit stops, then every (last - previous) */
} RenderOptions;
extern RenderOptions render;

enum { ATTR_REVERSE = 1, ATTR_BOLD = 2, ATTR_UNDERLINE = 4 };

typedef struct {
    size_t begin, end;    /* the line's bytes, without its newline */
    int header_cols;      /* columns kept in place while shifting */
    int shift;            /* columns hidden after the header columns */
    int width;            /* screen columns available */
    const Search *search; /* highlight all matches, or NULL */
    size_t only_from, only_to; /* with search NULL: highlight just these bytes */
    int line_attr;        /* applied to every glyph */
    int rscroll;          /* character marking a truncated line, 0 for none */
} LineRequest;

void render_setup(void);
/* Writes one screen line and returns the columns used. */
int render_line(FILE *out, const LineRequest *r);
/* Screen column of byte pos on the line [begin, end). */
int column_of(size_t begin, size_t end, size_t pos);
/* Columns used by the whole line, examining at most `limit` bytes. */
int line_width(size_t begin, size_t end, size_t limit);
int next_tab(int col);
/* -x: explicit stops; a single value n means a stop every n columns. */
void render_tabs(const int *tabs, int count);
/* Writes the display form of a byte string, for prompts and messages, and
   returns its columns; with out NULL it only measures. */
int render_text(FILE *out, const char *text, size_t len);
#endif
