#ifndef FASTLESS_SEARCH_H
#define FASTLESS_SEARCH_H
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <regex.h>
#include "literal.h"

/* No line index and no whole-file heap allocation. All positions are bytes
   in the current document, data[0..size). */
typedef enum { REGEX, LITERAL, GLOB } Mode;
typedef struct {
    Mode mode;
    char *text;
    size_t len;
    Literal fast;
    char *literal; /* owned text of `fast` when it is not the whole pattern */
    bool required, regex_prefix, regex_literal, glob_contains;
    bool fold, compiled;
    int subpattern; /* regex group a match must include, as for less's ^S; 0: whole match */
    regex_t re;
} Search;

extern const unsigned char *data;
extern size_t size;
extern volatile sig_atomic_t interrupted;
extern unsigned search_threads;

size_t line_end(size_t p);
size_t line_start(size_t p);
size_t next_line(size_t p);
size_t previous_line(size_t p);

void clear_search(Search *s);
/* case_mode: 0 sensitive, 1 less -i smart case, 2 -I. */
bool set_search(Search *s, Mode mode, const char *text, int case_mode, int subpattern,
                char *error, size_t cap);
/* Return 1 for a match, 0 for none, -1 for cancellation/error. Forward hits
   lie in [start, limit); backward hits start before `before`. */
int search_forward(const Search *s, size_t start, size_t limit,
                   size_t *hit, char *error, size_t cap);
int search_backward(const Search *s, size_t before, size_t *hit, char *error, size_t cap);
/* Whether a match starts on the line [begin, end). */
int line_matches(const Search *s, size_t begin, size_t end);
/* The first line at or after the line start `from` (or the last before
   `before`) holding no match. */
int nonmatch_forward(const Search *s, size_t from, size_t *hit, char *error, size_t cap);
int nonmatch_backward(const Search *s, size_t before, size_t *hit, char *error, size_t cap);
/* End of the match that starts at hit. */
size_t match_end(const Search *s, size_t hit);
/* Sets marks[i] for each byte from+i of [from, to) inside a match on the
   line [begin, end); matches outside the range count where they overlap. */
void search_marks(const Search *s, size_t begin, size_t end, size_t from, size_t to,
                  unsigned char *marks);

/* Line numbers, counted on demand with every search thread and cached per
   64 MiB piece of the current document. Lookups return 1, or 0 beyond the
   end, or -1 when interrupted. lines_reset() forgets the counts after the
   document changes; detach/attach keep a document's counts while another
   is current. Counts of complete pieces stay valid while a file grows. */
typedef struct LineCache LineCache;
void lines_reset(void);
LineCache *lines_detach(void);
/* `appended`: the document can only have grown at its end since detaching,
   so its counts still hold; otherwise they are kept only at the same size. */
void lines_attach(LineCache *cache, bool appended);
void lines_free(LineCache *cache);
int line_position(size_t number, size_t *pos); /* start of 1-based line */
int line_number(size_t pos, size_t *number);   /* 1-based line holding pos */
/* Without blocking: 1 if the number is known or cheap to find, 0 if pieces
   before pos must be counted first, -1 if interrupted. */
int line_number_known(size_t pos, size_t *number);
/* Counts about `bytes` more of the pieces before pos; true while some remain. */
bool lines_count_more(size_t pos, size_t bytes);
/* Fraction of the pieces before pos that are counted. */
double lines_progress(size_t pos);
#endif
