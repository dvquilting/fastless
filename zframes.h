#ifndef FASTLESS_ZFRAMES_H
#define FASTLESS_ZFRAMES_H
#include "ordered.h"

/* Decodes zstd input of several frames on `threads` threads. Returns 1,
   having done nothing, when the input is better decoded on one thread: a
   single frame, too little input, or anything the frame walk rejects, which
   the one-thread decoder then reports. Otherwise returns 0, -1 for invalid
   data (with a message), or -2 when the sink stopped it or *cancel was set. */
int zstd_frames_decode(const unsigned char *source, size_t length, unsigned threads,
                       volatile sig_atomic_t *cancel, OrderedSink sink, OrderedPlace place, void *context,
                       char *error, size_t error_size);
#endif
