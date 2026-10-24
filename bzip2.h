#ifndef FASTLESS_BZIP2_H
#define FASTLESS_BZIP2_H
#include <signal.h>
#include <stddef.h>

/* Receives decoded bytes in order; `consumed` is how much of the input they
   come from. Returns nonzero to stop decoding. */
typedef int (*Bzip2Sink)(void *context, const unsigned char *data, size_t length, size_t consumed);

/* Decodes concatenated bzip2 streams in source[0, length) on `threads`
   threads with lbzip2's decoder, verifying every block and stream CRC.
   Returns 0, -1 for invalid or truncated data (with a message, without the
   format's name), or -2 when the sink stopped it or *cancel was set. */
int bzip2_decode(const unsigned char *source, size_t length, unsigned threads,
                 volatile sig_atomic_t *cancel, Bzip2Sink sink, void *context,
                 char *error, size_t error_size);
#endif
