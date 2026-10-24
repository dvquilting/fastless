#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include "render.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

RenderOptions render = {.tabs = {8}, .tab_count = 1};
/* Zero-width bytes examined past the visible text before giving up, so a
   line of endless color codes cannot stall a redraw. */
#define SCAN_LIMIT ((size_t)1 << 20)

/* wcwidth needs a UTF-8 LC_CTYPE, but regexec must keep the C locale, so a
   UTF-8 locale is only made current around each wcwidth call. */
static locale_t utf8_locale;
void render_setup(void) {
    const char *names[] = {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8", ""};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !utf8_locale; ++i)
        utf8_locale = newlocale(LC_CTYPE_MASK, names[i], (locale_t)0);
}
/* Used when no UTF-8 locale exists: combining marks and wide East Asian
   ranges, enough for alignment in common text. */
static int fallback_width(uint32_t c) {
    static const uint32_t zero[][2] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
        {0x064B, 0x065F}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x1AB0, 0x1AFF},
        {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
        {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}};
    static const uint32_t wide[][2] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
        {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xAC00, 0xD7A3},
        {0xF900, 0xFAFF}, {0xFE30, 0xFE4F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
        {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD}};
    for (size_t i = 0; i < sizeof(zero) / sizeof(zero[0]); ++i)
        if (c >= zero[i][0] && c <= zero[i][1]) return 0;
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); ++i)
        if (c >= wide[i][0] && c <= wide[i][1]) return 2;
    return 1;
}
static int char_width(uint32_t c) {
    if (c < 0xA0) return c >= 0x20 && c < 0x7F ? 1 : -1;
    if (c < 0x300) return 1;
    if (!utf8_locale) return fallback_width(c);
    locale_t old = uselocale(utf8_locale);
    int w = wcwidth((wchar_t)c);
    uselocale(old);
    return w;
}
/* Length of the well-formed UTF-8 sequence at p (no overlongs, surrogates,
   or values past U+10FFFF), or 0. */
static size_t utf8_decode(const unsigned char *p, size_t n, uint32_t *c) {
    unsigned char b = p[0];
    size_t len;
    uint32_t v, low = 0x80, high = 0xBF;
    if (b < 0x80) { *c = b; return 1; }
    if (b >= 0xC2 && b <= 0xDF) { len = 2; v = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) {
        len = 3; v = b & 0x0F;
        if (b == 0xE0) low = 0xA0;
        if (b == 0xED) high = 0x9F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4; v = b & 0x07;
        if (b == 0xF0) low = 0x90;
        if (b == 0xF4) high = 0x8F;
    } else return 0;
    if (n < len || p[1] < low || p[1] > high) return 0;
    v = v << 6 | (p[1] & 0x3F);
    for (size_t i = 2; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = v << 6 | (p[i] & 0x3F);
    }
    *c = v;
    return len;
}
void render_tabs(const int *tabs, int count) {
    if (count > 32) count = 32;
    memcpy(render.tabs, tabs, (size_t)count * sizeof(*tabs));
    render.tab_count = count;
}
int next_tab(int col) {
    for (int i = 0; i < render.tab_count; ++i)
        if (render.tabs[i] > col) return render.tabs[i];
    int last = render.tab_count ? render.tabs[render.tab_count - 1] : 0;
    int step = render.tab_count > 1 ? last - render.tabs[render.tab_count - 2] :
               render.tab_count ? last : 8;
    if (step <= 0) step = 8;
    return col < last ? last : last + ((col - last) / step + 1) * step;
}
enum { G_TEXT, G_SHOW, G_TAB, G_SGR, G_NONE };
typedef struct {
    size_t next;        /* first byte after the glyph */
    size_t from, to;    /* G_TEXT and G_SGR: the bytes to write */
    int kind, width, attr;
    char show[16];      /* G_SHOW: substitute text */
} Glyph;
/* A printable character, or a substitute for a byte that is not one. */
static void basic_glyph(size_t q, size_t end, int col, Glyph *g) {
    unsigned char c = data[q];
    *g = (Glyph){.next = q + 1, .from = q, .to = q + 1, .kind = G_TEXT, .width = 1};
    if (c == '\t' && !render.ctl_tab) {
        g->kind = G_TAB;
        g->width = next_tab(col) - col;
        return;
    }
    if (c == '\r' && !render.ctl_return && q + 1 == end && end < size) {
        g->kind = G_NONE; g->width = 0;
        return;
    }
    if (c == 0x1B && render.raw_color && q + 1 < end && data[q + 1] == '[') {
        size_t p = q + 2;
        while (p < end && p - q < 64 && (strchr("0123456789;:", data[p]) && data[p])) ++p;
        if (p < end && data[p] == 'm') {
            *g = (Glyph){.next = p + 1, .from = q, .to = p + 1, .kind = G_SGR};
            return;
        }
    }
    if (c < 0x20 || c == 0x7F) {
        g->kind = G_SHOW; g->width = 2; g->attr = ATTR_BOLD;
        g->show[0] = '^'; g->show[1] = (char)(c ^ 0x40);
        return;
    }
    if (c < 0x80) return;
    uint32_t cp = 0;
    size_t len = render.utf8 ? utf8_decode(data + q, end - q, &cp) : 0;
    int w = len ? char_width(cp) : -1;
    if (w >= 0) {
        g->next = g->to = q + len;
        g->width = w;
        return;
    }
    g->kind = G_SHOW; g->attr = ATTR_BOLD;
    if (len) {
        g->next = q + len;
        g->width = snprintf(g->show, sizeof(g->show), "<U+%04X>", (unsigned)cp);
    } else g->width = snprintf(g->show, sizeof(g->show), "<%02X>", c);
}
static bool same_text(const Glyph *a, const Glyph *b) {
    return a->to - a->from == b->to - b->from && !memcmp(data + a->from, data + b->from, a->to - a->from);
}
/* One displayed unit: less's overstrike rules turn "c\bc" into bold c and
   "_\bc" or "c\b_" into underlined c; any other "x\b" deletes x. */
static void glyph_at(size_t q, size_t end, int col, Glyph *g) {
    basic_glyph(q, end, col, g);
    if (render.ctl_backspace || g->kind != G_TEXT) return;
    while (g->next + 1 < end && data[g->next] == '\b') {
        Glyph over;
        basic_glyph(g->next + 1, end, col, &over);
        if (over.kind != G_TEXT) break;
        bool under = g->to - g->from == 1 && data[g->from] == '_';
        bool under2 = over.to - over.from == 1 && data[over.from] == '_';
        int attr = g->attr | over.attr;
        if (same_text(g, &over)) attr |= ATTR_BOLD;
        else if (under) { attr |= ATTR_UNDERLINE; *g = over; }
        else if (under2) { attr |= ATTR_UNDERLINE; g->next = over.next; }
        else *g = over;
        g->next = over.next;
        g->attr = attr;
    }
}
int column_of(size_t begin, size_t end, size_t pos) {
    int col = 0;
    for (size_t q = begin; q < end && q < pos;) {
        Glyph g;
        glyph_at(q, end, col, &g);
        if (g.next > pos) break;
        col += g.width;
        q = g.next;
    }
    return col;
}
int line_width(size_t begin, size_t end, size_t limit) {
    int col = 0;
    for (size_t q = begin; q < end && q - begin < limit && col < INT32_MAX / 2;) {
        Glyph g;
        glyph_at(q, end, col, &g);
        col += g.width;
        q = g.next;
    }
    return col;
}
typedef struct {
    FILE *out;
    int attr;           /* attributes currently set by fastless */
    char sgr[512];      /* file color codes since the last reset, replayed after ours */
    size_t sgr_len;
} Painter;
static void paint(Painter *p, int want) {
    if (want == p->attr) return;
    if (p->attr & ~want) {
        fputs("\033[0m", p->out);
        fwrite(p->sgr, 1, p->sgr_len, p->out);
        p->attr = 0;
    }
    int add = want & ~p->attr;
    if (add & ATTR_BOLD) fputs("\033[1m", p->out);
    if (add & ATTR_UNDERLINE) fputs("\033[4m", p->out);
    if (add & ATTR_REVERSE) fputs("\033[7m", p->out);
    p->attr = want;
}
static void file_color(Painter *p, size_t from, size_t to) {
    size_t n = to - from;
    fwrite(data + from, 1, n, p->out);
    if (n == 3 || (n == 4 && data[from + 2] == '0')) p->sgr_len = 0;
    else if (p->sgr_len + n <= sizeof(p->sgr)) { memcpy(p->sgr + p->sgr_len, data + from, n); p->sgr_len += n; }
    /* The file's codes may have reset ours. */
    int keep = p->attr;
    p->attr = 0;
    paint(p, keep);
}
static void emit(Painter *p, const Glyph *g, int attr) {
    if (g->kind == G_SGR) { file_color(p, g->from, g->to); return; }
    if (g->kind == G_NONE) return;
    paint(p, attr | g->attr);
    if (g->kind == G_TEXT) fwrite(data + g->from, 1, g->to - g->from, p->out);
    else if (g->kind == G_SHOW) fputs(g->show, p->out);
    else for (int i = 0; i < g->width; ++i) putc(' ', p->out);
}
typedef struct {
    size_t header_end, body_start, visible_end;
    int header_width, pad, body_width;
    bool truncated;
} Layout;
static void layout(const LineRequest *r, int width, Layout *l) {
    int header = r->header_cols < width ? r->header_cols : width;
    int col = 0;
    size_t q = r->begin;
    *l = (Layout){0};
    while (q < r->end && col < header) {
        Glyph g;
        glyph_at(q, r->end, col, &g);
        if (col + g.width > header) break;
        col += g.width; q = g.next;
    }
    l->header_end = q;
    l->header_width = col;
    /* Skip shifted columns; a wide glyph cut by the edge leaves blanks. */
    int left = header + r->shift;
    while (q < r->end && col < left) {
        Glyph g;
        glyph_at(q, r->end, col, &g);
        col += g.width; q = g.next;
    }
    l->body_start = q;
    l->pad = col > left ? col - left : 0;
    int room = width - header;
    int used = l->pad;
    size_t scanned = 0;
    while (q < r->end && scanned < SCAN_LIMIT) {
        Glyph g;
        glyph_at(q, r->end, col, &g);
        if (used + g.width > room) { l->truncated = true; break; }
        if (!g.width) scanned += g.next - q;
        else scanned = 0;
        used += g.width; col += g.width; q = g.next;
    }
    l->visible_end = q;
    l->body_width = used;
    /* Zero-width trailers (a hidden CR, color codes) do not truncate. */
    if (l->truncated) {
        l->truncated = false;
        for (size_t t = q, n = 0; t < r->end && n < 4096;) {
            Glyph g;
            glyph_at(t, r->end, col, &g);
            if (g.width) { l->truncated = true; break; }
            n += g.next - t; t = g.next;
        }
    }
}
static unsigned char *span_marks(const LineRequest *r, size_t from, size_t to) {
    if (to <= from || (!r->search && r->only_to <= r->only_from)) return NULL;
    unsigned char *marks = malloc(to - from);
    if (!marks) return NULL;
    if (r->search) search_marks(r->search, r->begin, r->end, from, to, marks);
    else
        for (size_t i = from; i < to; ++i) marks[i - from] = i >= r->only_from && i < r->only_to;
    return marks;
}
static bool marked(const unsigned char *marks, size_t base, size_t from, size_t to) {
    if (!marks) return false;
    for (size_t i = from; i < to; ++i)
        if (marks[i - base]) return true;
    return false;
}
int render_line(FILE *out, const LineRequest *r) {
    if (r->width <= 0) return 0;
    Layout l;
    layout(r, r->width, &l);
    if (l.truncated && r->rscroll && r->width > 1) layout(r, r->width - 1, &l);
    Painter p = {.out = out};
    unsigned char *head = span_marks(r, r->begin, l.header_end);
    unsigned char *body = span_marks(r, l.body_start, l.visible_end);
    int col = 0;
    for (size_t q = r->begin; q < l.header_end;) {
        Glyph g;
        glyph_at(q, r->end, col, &g);
        emit(&p, &g, r->line_attr | (marked(head, r->begin, q, g.next) ? ATTR_REVERSE : 0));
        col += g.width; q = g.next;
    }
    int header = r->header_cols < r->width ? r->header_cols : r->width;
    int used = l.header_width;
    if (l.body_width) {
        paint(&p, r->line_attr);
        for (; used < header; ++used) putc(' ', out);
    }
    /* Colors set in the hidden part still apply to what is shown. */
    for (size_t q = l.header_end; q < l.body_start;) {
        Glyph g;
        glyph_at(q, r->end, col, &g);
        if (g.kind == G_SGR) file_color(&p, g.from, g.to);
        col += g.width; q = g.next;
    }
    paint(&p, r->line_attr);
    for (int i = 0; i < l.pad; ++i) putc(' ', out);
    used += l.pad;
    for (size_t q = l.body_start; q < l.visible_end;) {
        Glyph g;
        glyph_at(q, r->end, col, &g);
        emit(&p, &g, r->line_attr | (marked(body, l.body_start, q, g.next) ? ATTR_REVERSE : 0));
        col += g.width; used += g.width; q = g.next;
    }
    if (l.truncated && r->rscroll && r->width > 1) {
        paint(&p, r->line_attr);
        for (; used < r->width - 1; ++used) putc(' ', out);
        paint(&p, ATTR_REVERSE);
        putc(r->rscroll, out);
        ++used;
    }
    if (p.attr || p.sgr_len) fputs("\033[0m", out);
    free(head);
    free(body);
    return used;
}
int render_text(FILE *out, const char *text, size_t len) {
    int cols = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp;
        size_t n = c >= 0x80 && render.utf8 ? utf8_decode((const unsigned char *)text + i, len - i, &cp) : 0;
        int w = n ? char_width(cp) : -1;
        if (c >= 0x20 && c < 0x7F) { if (out) putc(c, out); ++cols; ++i; }
        else if (w >= 0) { if (out) fwrite(text + i, 1, n, out); cols += w; i += n; }
        else if (c < 0x20 || c == 0x7F) { if (out) { putc('^', out); putc(c ^ 0x40, out); } cols += 2; ++i; }
        else { if (out) fprintf(out, "<%02X>", c); cols += 4; ++i; }
    }
    return cols;
}
