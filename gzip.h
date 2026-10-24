#ifndef FASTLESS_GZIP_H
#define FASTLESS_GZIP_H
#include "ordered.h"

/* Inputs shorter than this decode faster on one thread. */
size_t gzip_parallel_minimum(void);

/* Decodes concatenated gzip members in source[0, length) on `threads`
   threads, verifying every member's CRC-32 and length; `place` may be NULL.
   Returns 0, -1 for invalid or truncated data (with a message), or -2 when
   the sink stopped it or *cancel was set. */
int gzip_decode(const unsigned char *source, size_t length, unsigned threads,
                volatile sig_atomic_t *cancel, OrderedSink sink, OrderedPlace place, void *context,
                char *error, size_t error_size);
#endif
