/* gzip decoding on many threads, after rapidgzip (Knespel and Brunst,
   "Rapidgzip: Parallel Decompression and Seeking in Gzip Files Using Cache
   Prefetching", HPDC 2023).

   Nothing marks where deflate blocks start, and they need not start on a
   byte. So each worker takes a chunk of the input, searches it for the first
   bit position where a block header is valid and its block decodes, and
   decodes on from there without knowing the 32 KiB window before it. It
   decodes twice in step, with dictionaries holding lo and (lo + hi + 1) &
   255 at window position hi * 256 + lo: a byte copied from the window comes
   out different in the two, and the pair names its position. Once 32 KiB in
   a row agree, nothing later can reach the window, and the worker goes on
   with one decoding.

   A worker stops at the block boundary where the next chunk's worker
   started, which proves that start real: deflate decodes the same from a
   true boundary whatever the window. Passing that point instead means the
   next start was false, and the worker decodes through that chunk too.

   Each chunk's window is the end of the one before, which is known once
   that one is decoded and its own window is: only 32 KiB of it need
   filling in for that. Workers, and the main thread while it waits, then
   fill in the window bytes of whole pieces of output in parallel. The main
   thread takes the pieces in stream order and checks each member's CRC-32
   and length. */
#define _GNU_SOURCE
#include "gzip.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#ifdef FASTLESS_ISAL
#include <isa-l/igzip_lib.h>
#endif
#include "literal.h"
#if !defined(FASTLESS_NO_AVX2) && (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#define HAVE_AVX2_FILL 1
#include <immintrin.h>
#else
#define HAVE_AVX2_FILL 0
#endif

/* Output is passed on in pieces of GZIP_PIECE bytes, decoded GZIP_STEP at a
   time while the window is unknown; chunks are GZIP_CHUNK_MIN to
   GZIP_CHUNK_MAX bytes of input. Test builds make these tiny. */
#ifndef GZIP_PIECE
#define GZIP_PIECE ((size_t)1 << 20)
#endif
#ifndef GZIP_STEP
#define GZIP_STEP ((size_t)64 << 10)
#endif
#ifndef GZIP_CHUNK_MIN
#define GZIP_CHUNK_MIN ((size_t)1 << 20)
#endif
#ifndef GZIP_CHUNK_MAX
#define GZIP_CHUNK_MAX ((size_t)8 << 20)
#endif
#ifndef GZIP_BUDGET_PER_THREAD
#define GZIP_BUDGET_PER_THREAD ((size_t)32 << 20)
#endif
/* The chunk the main thread is reading may hold this many pieces beyond the
   budget, so that it never waits for chunks behind it. */
#define HEAD_PIECES 4
/* Decoded output per chunk aimed for, when the trailer suggests the ratio. */
#define CHUNK_OUTPUT ((size_t)8 << 20)
#define WINDOW 32768
/* Windows have slack for 4-byte gathers at their last byte. */
#define WINDOW_ALLOC (WINDOW + 4)
#define END SIZE_MAX
/* zlib takes input in runs of at most this many bytes. */
#define FEED ((size_t)1 << 30)

size_t gzip_parallel_minimum(void) { return 2 * GZIP_CHUNK_MIN; }

typedef struct Piece {
    struct Piece *next, *resolve_next;
    size_t chunk, at;         /* its chunk, and where in that chunk's output */
    unsigned char *data;
    const unsigned char *out; /* the finished output: `data`, or where it was placed */
    bool ready;               /* finished: window bytes filled in and placed */
    unsigned char *shadow;    /* bytes [0, marked) as the shadow decoding gave them */
    size_t length, marked, consumed;
    uint32_t crc;             /* of the data, once nothing is marked */
    bool member_end;          /* the member's trailer follows */
    uint32_t want_crc, want_size;
} Piece;

enum { START_NONE, START_BLOCK, START_MEMBER };
typedef struct { int kind; uint64_t at; } Start; /* `at` in bits */

typedef struct {
    bool claimed, found, done;
    /* A worker that passed this start without stopping there sets `killed`,
       a request to stop: a garbage decoding can pass a true start too. Once
       accepted, by stopping exactly there, a chunk always finishes. */
    bool killed, accepted, aborted, discarded, failed;
    Start start;
    Piece *first, *last;      /* output waiting for the main thread */
    size_t queued;            /* how many pieces that is */
    size_t next;              /* the chunk the output continues in, or END */
    bool ended;               /* it decoded to the end of the input */
    /* The 32 KiB before the output and where the output goes, once known. */
    unsigned char *window;
    size_t offset, produced;
    /* The output's last 32 KiB as decoded, which gives the next window. */
    unsigned char *tail, *tail_shadow;
    size_t tail_length;
    char error[128];
} Chunk;

typedef struct {
    const unsigned char *source;
    size_t length, size;      /* input bytes, and input bytes per chunk */
    size_t count, claim;      /* chunks, and the first that may be unclaimed */
    Chunk *chunks;
    volatile sig_atomic_t *cancel;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    size_t head;              /* the chunk the main thread is reading */
    size_t frontier;          /* the last chunk in stream order with its window */
    Piece *resolve_first, *resolve_last; /* pieces that can now be finished */
    OrderedPlace place;
    void *context;
    /* Output held is limited to `budget` bytes, a piece with a shadow
       counting double, except for the head's first HEAD_PIECES. */
    size_t buffered, budget;
    Piece *spare, *spare_marked; /* reusable pieces, without and with shadows */
    bool stop, broken;
} Pool;

/* Dictionaries naming the window position of each byte, set up once. */
static unsigned char low_dict[WINDOW], shift_dict[WINDOW];
static pthread_once_t dicts_once = PTHREAD_ONCE_INIT;
static void make_dicts(void) {
    for (size_t i = 0; i < WINDOW; ++i) {
        low_dict[i] = (unsigned char)i;
        shift_dict[i] = (unsigned char)(i + 1 + (i >> 8));
    }
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* The input's bits from bit `pos` on, at least 57 of them, zero past its end. */
static uint64_t peek(const unsigned char *s, size_t n, uint64_t pos) {
    size_t at = (size_t)(pos >> 3);
    uint64_t w = 0;
    if (at < n) {
        size_t have = n - at < 8 ? n - at : 8;
        for (size_t i = 0; i < have; ++i) w |= (uint64_t)s[at + i] << (8 * i);
    }
    return w >> (pos & 7);
}

/* zlib's rules for Huffman code lengths: -1 over-subscribed, 1 incomplete,
   0 complete; *longest is the longest code. */
static int kraft(const unsigned char *lengths, unsigned n, unsigned *longest) {
    unsigned count[16] = {0};
    *longest = 0;
    for (unsigned i = 0; i < n; ++i) {
        count[lengths[i]]++;
        if (lengths[i] > *longest) *longest = lengths[i];
    }
    int left = 1;
    for (unsigned len = 1; len < 16; ++len) {
        left = 2 * left - (int)count[len];
        if (left < 0) return -1;
    }
    return left > 0;
}
/* Whether a dynamic, non-final block header that zlib would accept starts
   at bit `pos`. Most positions fail on the first few bits. */
static bool dynamic_header(const unsigned char *s, size_t n, uint64_t pos) {
    static const unsigned char order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint64_t w = peek(s, n, pos);
    if ((w & 7) != 4) return false;
    unsigned hlit = (unsigned)(w >> 3) & 31, hdist = (unsigned)(w >> 8) & 31;
    if (hlit > 29 || hdist > 29) return false;
    unsigned hclen = ((unsigned)(w >> 13) & 15) + 4;
    pos += 17;
    uint64_t v = peek(s, n, pos);
    unsigned char cl[19] = {0};
    int left = 128;
    for (unsigned i = 0; i < hclen; ++i) {
        unsigned len = (unsigned)(v >> (3 * i)) & 7;
        cl[order[i]] = (unsigned char)len;
        if (len && (left -= 128 >> len) < 0) return false;
    }
    if (left) return false; /* zlib requires a complete code-length code */
    pos += 3 * hclen;
    /* A table of the code-length code, indexed by the next 7 input bits. */
    unsigned short table[128];
    unsigned code = 0, next[8] = {0}, count[8] = {0};
    for (unsigned i = 0; i < 19; ++i) count[cl[i]]++;
    count[0] = 0;
    for (unsigned len = 1; len < 8; ++len) { code = (code + count[len - 1]) << 1; next[len] = code; }
    for (unsigned sym = 0; sym < 19; ++sym) {
        unsigned len = cl[sym];
        if (!len) continue;
        unsigned c = next[len]++, r = 0;
        for (unsigned b = 0; b < len; ++b) r |= ((c >> b) & 1) << (len - 1 - b);
        for (unsigned k = r; k < 128; k += 1u << len) table[k] = (unsigned short)(sym | len << 8);
    }
    unsigned char lengths[286 + 30];
    unsigned total = hlit + 257 + hdist + 1;
    for (unsigned i = 0; i < total;) {
        if (pos > (uint64_t)n * 8) return false;
        v = peek(s, n, pos);
        unsigned entry = table[v & 127], sym = entry & 255, len = entry >> 8;
        pos += len;
        v >>= len;
        unsigned repeat;
        unsigned char value = 0;
        if (sym < 16) { lengths[i++] = (unsigned char)sym; continue; }
        if (sym == 16) {
            if (!i) return false;
            value = lengths[i - 1];
            repeat = 3 + (unsigned)(v & 3);
            pos += 2;
        } else if (sym == 17) {
            repeat = 3 + (unsigned)(v & 7);
            pos += 3;
        } else {
            repeat = 11 + (unsigned)(v & 127);
            pos += 7;
        }
        if (repeat > total - i) return false;
        memset(lengths + i, value, repeat);
        i += repeat;
    }
    if (pos > (uint64_t)n * 8 || !lengths[256]) return false;
    unsigned longest;
    int k = kraft(lengths, hlit + 257, &longest);
    if (k < 0 || (k > 0 && longest != 1)) return false;
    k = kraft(lengths + hlit + 257, hdist + 1, &longest);
    return k == 0 || (k > 0 && longest <= 1);
}
/* The length of a valid gzip member header at s[at], or 0. */
static size_t member_header(const unsigned char *s, size_t n, size_t at) {
    if (n - at < 10 || s[at] != 0x1f || s[at + 1] != 0x8b || s[at + 2] != 8 || (s[at + 3] & 0xe0)) return 0;
    unsigned flags = s[at + 3];
    size_t p = at + 10;
    if (flags & 4) {
        if (n - p < 2) return 0;
        size_t extra = (size_t)s[p] | (size_t)s[p + 1] << 8;
        p += 2;
        if (n - p < extra) return 0;
        p += extra;
    }
    for (unsigned flag = 8; flag <= 16; flag <<= 1)
        if (flags & flag) {
            const unsigned char *z = memchr(s + p, 0, n - p);
            if (!z) return 0;
            p = (size_t)(z - s) + 1;
        }
    if (flags & 2) {
        if (n - p < 2) return 0;
        if (((unsigned)s[p] | (unsigned)s[p + 1] << 8) != (crc32(0, s + at, (uInt)(p - at)) & 0xffff)) return 0;
        p += 2;
    }
    return p - at;
}

/* Starts a raw decoding at bit `pos`. */
static void start_at(z_stream *z, const unsigned char *s, uint64_t pos) {
    size_t at = (size_t)(pos >> 3);
    unsigned skip = (unsigned)(pos & 7);
    if (skip) { inflatePrime(z, (int)(8 - skip), s[at] >> skip); at++; }
    z->next_in = (Bytef *)(uintptr_t)(s + at);
    z->avail_in = 0;
}
static void feed(z_stream *z, const unsigned char *s, size_t n) {
    if (z->avail_in) return;
    size_t at = (size_t)(z->next_in - s);
    z->avail_in = (uInt)(n - at < FEED ? n - at : FEED);
}
static bool input_done(const z_stream *z, const unsigned char *s, size_t n) {
    return !z->avail_in && (size_t)(z->next_in - s) == n;
}
/* Whether the block at bit `pos` decodes to its end, followed by a valid
   block header or the end of the member. A member's first block (`fresh`)
   cannot reach back before it. */
static bool trial(const unsigned char *s, size_t n, uint64_t pos, bool fresh, unsigned char *scratch) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) return false;
    if (!fresh) inflateSetDictionary(&z, low_dict, WINDOW);
    start_at(&z, s, pos);
    bool ok = false, ended = false;
    /* Encoders end a block after at most 32 Ki symbols, 8.5 MB of output
       (GNU gzip, or zlib at memLevel 9); garbage is cut off well past that. */
    for (size_t out = 0; out < ((size_t)32 << 20);) {
        feed(&z, s, n);
        z.next_out = scratch;
        z.avail_out = (uInt)GZIP_STEP;
        int rc = inflate(&z, ended ? Z_TREES : Z_BLOCK);
        out += GZIP_STEP - z.avail_out;
        if (rc == Z_STREAM_END) { ok = true; break; }
        if (ended && (z.data_type & 256)) { ok = true; break; }
        if (rc != Z_OK) break;
        if (z.data_type & 128) ended = true;
    }
    inflateEnd(&z);
    return ok;
}
/* The first start in input bytes [from, to), checking for `stop` now and
   then. */
static Start find_start(Pool *p, size_t from, size_t to, unsigned char *scratch) {
    const unsigned char *s = p->source;
    size_t n = p->length;
    for (size_t q = from; q < to; ++q) {
        if (!(q & 65535)) {
            bool stop;
            pthread_mutex_lock(&p->lock);
            stop = p->stop || *p->cancel;
            pthread_mutex_unlock(&p->lock);
            if (stop) break;
        }
        if (s[q] == 0x1f && n - q > 2 && s[q + 1] == 0x8b && s[q + 2] == 8) {
            size_t h = member_header(s, n, q);
            if (h && trial(s, n, 8 * (uint64_t)(q + h), true, scratch))
                return (Start){START_MEMBER, 8 * (uint64_t)q};
        }
        uint64_t w = peek(s, n, 8 * (uint64_t)q);
        for (unsigned bit = 0; bit < 8; ++bit, w >>= 1) {
            if ((w & 7) != 4 || ((w >> 3) & 31) > 29 || ((w >> 8) & 31) > 29) continue;
            uint64_t pos = 8 * (uint64_t)q + bit;
            if (dynamic_header(s, n, pos) && trial(s, n, pos, false, scratch))
                return (Start){START_BLOCK, pos};
        }
    }
    return (Start){START_NONE, 0};
}


static void recycle(Pool *p, Piece *piece) {
    Piece **list = piece->shadow ? &p->spare_marked : &p->spare;
    p->buffered -= GZIP_PIECE + (piece->shadow ? GZIP_PIECE : 0);
    piece->next = *list;
    *list = piece;
}
/* Whether chunk n's worker should go on; called with the lock held. */
static bool alive(Pool *p, size_t n) {
    Chunk *c = &p->chunks[n];
    if (p->stop || *p->cancel || c->discarded || c->aborted) return false;
    if (c->killed && !c->accepted) { c->aborted = true; return false; }
    return true;
}
/* A piece to decode chunk n into, with room for the shadow decoding when
   `marked`, or NULL when the worker should stop, or with *failed set when
   there is no memory for one. */
static Piece *take_piece(Pool *p, size_t n, bool marked, bool *failed) {
    size_t cost = GZIP_PIECE + (marked ? GZIP_PIECE : 0);
    pthread_mutex_lock(&p->lock);
    while (alive(p, n) && p->buffered + cost > p->budget && !(n == p->head && p->chunks[n].queued < HEAD_PIECES))
        pthread_cond_wait(&p->changed, &p->lock);
    Piece *piece = NULL;
    if (alive(p, n)) {
        Piece **list = marked ? &p->spare_marked : &p->spare;
        if (*list) { piece = *list; *list = piece->next; }
        else if ((piece = calloc(1, sizeof(*piece)))) {
            piece->data = malloc(GZIP_PIECE);
            piece->shadow = marked ? malloc(GZIP_PIECE) : NULL;
            if (!piece->data || (marked && !piece->shadow)) {
                free(piece->data); free(piece->shadow); free(piece);
                piece = NULL;
            }
        }
        if (!piece) *failed = true;
        if (piece) {
            p->buffered += cost;
            unsigned char *data = piece->data, *shadow = piece->shadow;
            *piece = (Piece){.chunk = n, .data = data, .shadow = shadow};
        }
    }
    pthread_mutex_unlock(&p->lock);
    return piece;
}
/* What a byte decoded as `value` with low_dict and `shadow` with
   shift_dict is, given the window: itself where they agree, otherwise the
   window byte they name. Branch-free, as both kinds mix closely. */
static inline unsigned char resolve(const unsigned char *window, unsigned value, unsigned shadow) {
    unsigned char named = window[(((shadow - value - 1) & 127) << 8) | value];
    return value != shadow ? named : (unsigned char)value;
}
#if HAVE_AVX2_FILL
/* 8 bytes' resolve() lookups as 32-bit lanes, each holding the byte low. */
__attribute__((target("avx2"))) static inline __m256i gather8(const unsigned char *window, __m128i value, __m128i shadow) {
    __m256i v = _mm256_cvtepu8_epi32(value), s = _mm256_cvtepu8_epi32(shadow);
    __m256i hi = _mm256_and_si256(_mm256_sub_epi32(_mm256_sub_epi32(s, v), _mm256_set1_epi32(1)), _mm256_set1_epi32(127));
    __m256i index = _mm256_or_si256(_mm256_slli_epi32(hi, 8), v);
    return _mm256_and_si256(_mm256_i32gather_epi32((const int *)(const void *)window, index, 1), _mm256_set1_epi32(255));
}
/* fill()'s loop, 32 bytes at a time. */
__attribute__((target("avx2"))) static size_t fill_avx2(unsigned char *out, const unsigned char *data,
                                                        const unsigned char *shadow, size_t n,
                                                        const unsigned char *window) {
    size_t j = 0;
    for (; j + 32 <= n; j += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(data + j));
        __m256i b = _mm256_loadu_si256((const __m256i *)(const void *)(shadow + j));
        __m256i same = _mm256_cmpeq_epi8(a, b);
        if ((unsigned)_mm256_movemask_epi8(same) == 0xffffffffu) {
            if (out != data) _mm256_storeu_si256((__m256i *)(void *)(out + j), a);
            continue;
        }
        __m128i alo = _mm256_castsi256_si128(a), ahi = _mm256_extracti128_si256(a, 1);
        __m128i blo = _mm256_castsi256_si128(b), bhi = _mm256_extracti128_si256(b, 1);
        __m256i g0 = gather8(window, alo, blo), g1 = gather8(window, _mm_srli_si128(alo, 8), _mm_srli_si128(blo, 8));
        __m256i g2 = gather8(window, ahi, bhi), g3 = gather8(window, _mm_srli_si128(ahi, 8), _mm_srli_si128(bhi, 8));
        /* Packing interleaves the 128-bit lanes; the permute restores order. */
        __m256i packed = _mm256_packus_epi16(_mm256_packus_epi32(g0, g1), _mm256_packus_epi32(g2, g3));
        packed = _mm256_permutevar8x32_epi32(packed, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256((__m256i *)(void *)(out + j), _mm256_blendv_epi8(packed, a, same));
    }
    return j;
}
#endif
/* Writes a piece's output to `out`, which may be its data, with window
   bytes filled in, and works out its CRC. */
static void fill(Piece *piece, unsigned char *out, const unsigned char *window) {
    const unsigned char *data = piece->data, *shadow = piece->shadow;
    size_t n = piece->marked, j = 0;
#if HAVE_AVX2_FILL
    if (literal_avx2_enabled()) j = fill_avx2(out, data, shadow, n, window);
#endif
    for (; j < n; ++j) out[j] = resolve(window, data[j], shadow[j]);
    if (out != data) memcpy(out + n, data + n, piece->length - n);
    piece->crc = (uint32_t)crc32(0, out, (uInt)piece->length);
}
/* Finishes a piece once its chunk's window and output offset are known:
   fills in window bytes as it copies the piece into place, which spreads
   that work, and faulting in the output's pages, over the threads. */
static void finish(Pool *p, Piece *piece, const unsigned char *window, size_t offset) {
    unsigned char *at = p->place && piece->length ? p->place(p->context, offset + piece->at, piece->length) : NULL;
    piece->out = at ? at : piece->data;
    if (piece->marked) fill(piece, at ? at : piece->data, window);
    else if (at) memcpy(at, piece->data, piece->length);
}
/* Finishes one piece waiting for that, if any; called with the lock held,
   which it releases meanwhile. */
static bool help(Pool *p) {
    Piece *piece = p->resolve_first;
    if (!piece) return false;
    p->resolve_first = piece->resolve_next;
    if (!p->resolve_first) p->resolve_last = NULL;
    const Chunk *c = &p->chunks[piece->chunk];
    const unsigned char *window = c->window;
    size_t offset = c->offset;
    pthread_mutex_unlock(&p->lock);
    finish(p, piece, window, offset);
    pthread_mutex_lock(&p->lock);
    piece->marked = 0;
    piece->ready = true;
    pthread_cond_broadcast(&p->changed);
    return true;
}
/* Keeps the last 32 KiB of a chunk's output, as decoded. */
static void keep_tail(Chunk *c, const Piece *piece) {
    size_t n = piece->length, from = n > WINDOW ? n - WINDOW : 0, take = n - from;
    size_t keep = c->tail_length + take > WINDOW ? WINDOW - take : c->tail_length;
    memmove(c->tail, c->tail + c->tail_length - keep, keep);
    memmove(c->tail_shadow, c->tail_shadow + c->tail_length - keep, keep);
    memcpy(c->tail + keep, piece->data + from, take);
    /* Past `marked`, the output is its own shadow. */
    size_t marked = piece->marked > from ? piece->marked - from : 0;
    if (marked > take) marked = take;
    if (marked) memcpy(c->tail_shadow + keep, piece->shadow + from, marked);
    memcpy(c->tail_shadow + keep + marked, piece->data + from + marked, take - marked);
    c->tail_length = keep + take;
}
static void give(Pool *p, size_t n, Piece *piece) {
    Chunk *c = &p->chunks[n];
    keep_tail(c, piece);
    if (!piece->marked) piece->crc = (uint32_t)crc32(0, piece->data, (uInt)piece->length);
    pthread_mutex_lock(&p->lock);
    if (c->window && !c->discarded) {
        /* The window and offset are known by now: finish it here. */
        const unsigned char *window = c->window;
        size_t offset = c->offset;
        pthread_mutex_unlock(&p->lock);
        finish(p, piece, window, offset);
        pthread_mutex_lock(&p->lock);
        piece->marked = 0;
        piece->ready = true;
    }
    if (c->discarded) recycle(p, piece);
    else {
        piece->next = NULL;
        if (c->last) c->last->next = piece;
        else c->first = piece;
        c->last = piece;
        c->queued++;
    }
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
}
/* Works out the windows of chunks in stream order as far as their
   predecessors are decoded; called with the lock held. */
static void advance(Pool *p) {
    for (;;) {
        Chunk *c = &p->chunks[p->frontier];
        if (!c->done || c->failed || c->aborted || c->next == END || !c->window) return;
        Chunk *d = &p->chunks[c->next];
        if (!(d->window = calloc(1, WINDOW_ALLOC))) { p->broken = true; return; }
        d->offset = c->offset + c->produced;
        size_t keep = WINDOW - c->tail_length;
        memcpy(d->window, c->window + WINDOW - keep, keep);
        for (size_t j = 0; j < c->tail_length; ++j)
            d->window[keep + j] = resolve(c->window, c->tail[j], c->tail_shadow[j]);
        free(c->tail);
        free(c->tail_shadow);
        c->tail = NULL;
        c->tail_shadow = NULL;
        for (Piece *piece = d->first; piece; piece = piece->next)
            if (!piece->ready) {
                piece->resolve_next = NULL;
                if (p->resolve_last) p->resolve_last->resolve_next = piece;
                else p->resolve_first = piece;
                p->resolve_last = piece;
            }
        p->frontier = c->next;
        pthread_cond_broadcast(&p->changed);
    }
}
/* At a block boundary or member header at bit `pos` of chunk n's decoding,
   with *target the next chunk it could hand over to: 1 when the output
   continues in chunk *target from here, 0 to decode on, -1 to stop. */
static int boundary(Pool *p, size_t n, size_t *target, uint64_t pos) {
    if (*target >= p->count || pos < 8 * (uint64_t)(*target * p->size)) return 0;
    pthread_mutex_lock(&p->lock);
    int result = 0;
    for (;;) {
        if (!alive(p, n)) { result = -1; break; }
        size_t m = *target;
        if (m >= p->count || pos < 8 * (uint64_t)(m * p->size)) break;
        Chunk *c = &p->chunks[m];
        if (!c->claimed) {
            /* No worker got to it: it is decoded here. */
            c->claimed = c->found = c->done = true;
            c->start.kind = START_NONE;
            ++*target;
            continue;
        }
        /* Still searching: decide at a later boundary rather than wait. */
        if (!c->found) break;
        if (c->start.kind == START_NONE || pos > c->start.at) {
            if (!c->accepted) c->killed = true;
            pthread_cond_broadcast(&p->changed);
            ++*target;
            continue;
        }
        if (pos == c->start.at && !c->aborted) { c->accepted = true; result = 1; }
        else if (pos == c->start.at) { ++*target; continue; }
        break;
    }
    pthread_mutex_unlock(&p->lock);
    return result;
}

/* The shadow decoding, with shift_dict: ISA-L when it is linked, being
   faster, otherwise zlib. It need only give as many bytes as the main one
   when asked, without stopping at blocks. */
typedef struct {
#ifdef FASTLESS_ISAL
    struct inflate_state *isal;
#else
    z_stream z;
#endif
    bool live;
} Shadow;
static bool shadow_start(Shadow *sh, const unsigned char *s, uint64_t pos) {
    size_t at = (size_t)(pos >> 3);
    unsigned skip = (unsigned)(pos & 7);
#ifdef FASTLESS_ISAL
    if (!(sh->isal = malloc(sizeof(*sh->isal)))) return false;
    sh->live = true;
    isal_inflate_init(sh->isal);
    sh->isal->crc_flag = ISAL_DEFLATE;
    if (isal_inflate_set_dict(sh->isal, shift_dict, WINDOW) != COMP_OK) return false;
    /* A start inside a byte goes in ISA-L's buffer of unread bits. */
    if (skip) {
        sh->isal->read_in = s[at] >> skip;
        sh->isal->read_in_length = (int32_t)(8 - skip);
        at++;
    }
    sh->isal->next_in = (uint8_t *)(uintptr_t)(s + at);
    sh->isal->avail_in = 0;
    return true;
#else
    (void)at; (void)skip;
    memset(&sh->z, 0, sizeof(sh->z));
    if (inflateInit2(&sh->z, -15) != Z_OK) return false;
    sh->live = true;
    inflateSetDictionary(&sh->z, shift_dict, WINDOW);
    start_at(&sh->z, s, pos);
    return true;
#endif
}
/* Decodes exactly `want` bytes, or fails. */
static bool shadow_step(Shadow *sh, const unsigned char *s, size_t n, unsigned char *out, size_t want) {
#ifdef FASTLESS_ISAL
    struct inflate_state *z = sh->isal;
    z->next_out = out;
    z->avail_out = (uint32_t)want;
    while (z->avail_out) {
        if (!z->avail_in) {
            size_t at = (size_t)(z->next_in - s);
            z->avail_in = (uint32_t)(n - at < FEED ? n - at : FEED);
        }
        uint32_t in = z->avail_in, left = z->avail_out;
        if (isal_inflate(z) < 0) return false;
        if (z->block_state == ISAL_BLOCK_FINISH || (in == z->avail_in && left == z->avail_out)) break;
    }
    return !z->avail_out;
#else
    z_stream *z = &sh->z;
    z->next_out = out;
    z->avail_out = (uInt)want;
    while (z->avail_out) {
        feed(z, s, n);
        uInt in = z->avail_in, left = z->avail_out;
        int rc = inflate(z, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if ((rc != Z_OK && rc != Z_BUF_ERROR) || (in == z->avail_in && left == z->avail_out)) break;
    }
    return !z->avail_out;
#endif
}
static void shadow_end(Shadow *sh) {
#ifdef FASTLESS_ISAL
    free(sh->isal);
    sh->isal = NULL;
#else
    if (sh->live) inflateEnd(&sh->z);
#endif
    sh->live = false;
}

/* Decodes chunk n from `st` until it hands over or the stream ends. */
static void run_chunk(Pool *p, size_t n, Start st) {
    const unsigned char *s = p->source;
    Chunk *chunk = &p->chunks[n];
    size_t len = p->length, target = n + 1, next = END, produced = 0, clean = 0;
    const char *error = NULL;
    char message[128] = "";
    bool handed = false, ended = false;
    z_stream z[1];
    memset(z, 0, sizeof(z));
    int live = 0;
    Shadow shadow = {0};
    Piece *piece = NULL;
    uint64_t pos = st.at;
    chunk->tail = malloc(WINDOW);
    chunk->tail_shadow = malloc(WINDOW);
    if (!chunk->tail || !chunk->tail_shadow) { error = "Cannot allocate gzip decoder"; goto finish; }
    if (inflateInit2(&z[0], -15) != Z_OK) { error = "Cannot allocate gzip decoder"; goto finish; }
    live = 1;
    if (st.kind == START_MEMBER) {
        size_t h = member_header(s, len, (size_t)(pos >> 3));
        if (!h) { error = len - (size_t)(pos >> 3) < 10 ? "Truncated or invalid gzip stream" : "Invalid gzip header"; goto finish; }
        pos += 8 * (uint64_t)h;
    } else {
        inflateSetDictionary(&z[0], low_dict, WINDOW);
        if (!shadow_start(&shadow, s, pos)) { error = "Cannot allocate gzip decoder"; goto finish; }
    }
    start_at(&z[0], s, pos);
    if (st.kind == START_MEMBER) {
        int b = boundary(p, n, &target, pos);
        if (b) { handed = b > 0; goto finish; }
    }
    for (;;) {
        if (!piece) {
            bool failed = false;
            if (!(piece = take_piece(p, n, shadow.live, &failed))) {
                if (failed) error = "Cannot allocate decompression buffer";
                goto finish;
            }
            piece->at = produced;
        }
        size_t room = GZIP_PIECE - piece->length;
        size_t step = shadow.live && room > GZIP_STEP ? GZIP_STEP : room;
        feed(&z[0], s, len);
        const Bytef *before = z[0].next_in;
        z[0].next_out = piece->data + piece->length;
        z[0].avail_out = (uInt)step;
        int rc = inflate(&z[0], Z_BLOCK);
        size_t got = step - z[0].avail_out;
        if (shadow.live) {
            const unsigned char *out = piece->data + piece->length, *other = piece->shadow + piece->length;
            /* The same input decodes the same way, whatever the window: the
               shadow gives as many bytes, and where they differ, they name
               a window position. */
            bool same = got ? shadow_step(&shadow, s, len, piece->shadow + piece->length, got) : true;
            unsigned char named = 0;
            for (size_t j = 0; same && j < got; ++j)
                named |= (unsigned char)((out[j] != other[j]) * (unsigned char)(other[j] - out[j] - 1));
            if (!same || (named & 128)) {
                /* Never seen; the chunk is left to the one before it. */
                pthread_mutex_lock(&p->lock);
                bool accepted = chunk->accepted;
                if (!accepted) chunk->aborted = true;
                pthread_mutex_unlock(&p->lock);
                if (accepted) error = "Internal gzip decoding mismatch";
                goto finish;
            }
            /* Where the last window byte is. */
            size_t j = got;
            while (j >= 8) {
                uint64_t a, b;
                memcpy(&a, out + j - 8, 8);
                memcpy(&b, other + j - 8, 8);
                if (a != b) break;
                j -= 8;
            }
            while (j && out[j - 1] == other[j - 1]) --j;
            if (j) clean = produced + j;
            piece->marked = piece->length + got;
            if (produced + got - clean >= WINDOW || rc == Z_STREAM_END) shadow_end(&shadow);
        }
        piece->length += got;
        produced += got;
        piece->consumed = (size_t)(z[0].next_in - s);
        if (rc == Z_STREAM_END) {
            size_t at = (size_t)(z[0].next_in - s);
            if (len - at < 8) { error = "Truncated or invalid gzip stream"; goto finish; }
            piece->member_end = true;
            piece->want_crc = le32(s + at);
            piece->want_size = le32(s + at + 4);
            at += 8;
            piece->consumed = at;
            give(p, n, piece);
            piece = NULL;
            /* GNU gzip permits zero padding after the last member. */
            if (at == len || !s[at]) {
                while (at < len && !s[at]) ++at;
                if (at < len) error = "Invalid trailing data after gzip member";
                ended = !error;
                goto finish;
            }
            int b = boundary(p, n, &target, 8 * (uint64_t)at);
            if (b) { handed = b > 0; goto finish; }
            size_t h = member_header(s, len, at);
            if (!h) { error = len - at < 10 ? "Truncated or invalid gzip stream" : "Invalid gzip header"; goto finish; }
            inflateReset(&z[0]);
            start_at(&z[0], s, 8 * (uint64_t)(at + h));
            b = boundary(p, n, &target, 8 * (uint64_t)(at + h));
            if (b) { handed = b > 0; goto finish; }
            continue;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            snprintf(message, sizeof(message), "%s", z[0].msg ? z[0].msg : "Invalid gzip stream");
            error = message;
            goto finish;
        }
        if (!got && input_done(&z[0], s, len)) { error = "Truncated or invalid gzip stream"; goto finish; }
        if (!got && z[0].next_in == before && !(z[0].data_type & 128)) { error = "Invalid gzip stream"; goto finish; }
        if (z[0].data_type & 128) {
            uint64_t b = (uint64_t)(z[0].next_in - s) * 8 - (unsigned)(z[0].data_type & 7);
            int r = boundary(p, n, &target, b);
            if (r) { handed = r > 0; goto finish; }
        }
        if (piece->length == GZIP_PIECE) { give(p, n, piece); piece = NULL; }
    }
finish:
    if (handed) next = target;
    if (piece && handed && piece->length) {
        give(p, n, piece);
        piece = NULL;
    }
    if (live) inflateEnd(&z[0]);
    shadow_end(&shadow);
    pthread_mutex_lock(&p->lock);
    if (piece) recycle(p, piece);
    chunk->next = next;
    chunk->ended = ended;
    chunk->produced = produced;
    if (error) {
        chunk->failed = true;
        snprintf(chunk->error, sizeof(chunk->error), "%s", error);
    }
    chunk->done = true;
    advance(p);
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
}

static void wait_changed(Pool *p) { ordered_wait(&p->changed, &p->lock); }
static void *worker(void *arg) {
    Pool *p = arg;
    unsigned char *scratch = malloc(GZIP_STEP); /* for trial decodings */
    pthread_mutex_lock(&p->lock);
    if (!scratch) {
        /* Otherwise the chunks it would have taken might never be. */
        p->broken = true;
        pthread_cond_broadcast(&p->changed);
    }
    while (scratch && !p->stop && !*p->cancel) {
        if (help(p)) continue;
        while (p->claim < p->count && p->chunks[p->claim].claimed) p->claim++;
        if (p->claim >= p->count) { wait_changed(p); continue; }
        size_t n = p->claim++;
        Chunk *c = &p->chunks[n];
        c->claimed = true;
        pthread_mutex_unlock(&p->lock);
        size_t from = n * p->size, to = p->length - from > p->size ? from + p->size : p->length;
        Start st = n ? find_start(p, from, to, scratch) : (Start){START_MEMBER, 0};
#ifdef GZIP_FALSE_STARTS
        /* Test builds move every third start a bit on, as if a false start
           had passed the checks, so that chunks are passed and aborted. */
        if (n % 3 == 0 && st.kind == START_BLOCK) st.at++;
#endif
        pthread_mutex_lock(&p->lock);
        c->start = st;
        c->found = true;
        pthread_cond_broadcast(&p->changed);
        if (st.kind == START_NONE) { c->done = true; continue; }
        pthread_mutex_unlock(&p->lock);
        run_chunk(p, n, st);
        pthread_mutex_lock(&p->lock);
    }
    pthread_mutex_unlock(&p->lock);
    free(scratch);
    return NULL;
}
static void discard(Pool *p, size_t n) {
    Chunk *c = &p->chunks[n];
    c->discarded = true;
    while (c->first) {
        Piece *piece = c->first;
        c->first = piece->next;
        recycle(p, piece);
    }
    c->last = NULL;
    c->queued = 0;
}
static void free_list(Piece *piece) {
    while (piece) {
        Piece *next = piece->next;
        free(piece->data);
        free(piece->shadow);
        free(piece);
        piece = next;
    }
}

int gzip_decode(const unsigned char *source, size_t length, unsigned threads,
                volatile sig_atomic_t *cancel, OrderedSink sink, OrderedPlace place, void *context,
                char *error, size_t error_size) {
    if (!threads) threads = 1;
    pthread_once(&dicts_once, make_dicts);
    /* Several chunks per thread balance the work, but each chunk costs a
       search and a start without the window. The trailer's length, modulo
       4 GiB, hints at the compression ratio. */
    size_t size = length / (4 * (size_t)threads);
    if (size > GZIP_CHUNK_MAX) size = GZIP_CHUNK_MAX;
    if (length >= 8) {
        uint32_t isize = le32(source + length - 4);
        if (isize > length && size > CHUNK_OUTPUT / (isize / length)) size = CHUNK_OUTPUT / (isize / length);
    }
    if (size < GZIP_CHUNK_MIN) size = GZIP_CHUNK_MIN;
    Pool p = {.source = source, .length = length, .size = size, .count = length ? (length - 1) / size + 1 : 1,
              .cancel = cancel, .budget = (size_t)threads * GZIP_BUDGET_PER_THREAD,
              .place = place, .context = context};
    p.chunks = calloc(p.count, sizeof(*p.chunks));
    pthread_t *workers = calloc(threads, sizeof(*workers));
    if (p.chunks) p.chunks[0].window = calloc(1, WINDOW_ALLOC);
    if (!p.chunks || !workers || !p.chunks[0].window) {
        if (p.chunks) free(p.chunks[0].window);
        free(p.chunks); free(workers);
        snprintf(error, error_size, "Cannot allocate decoder state");
        return -1;
    }
    pthread_mutex_init(&p.lock, NULL);
    pthread_cond_init(&p.changed, NULL);
    unsigned started = ordered_start(workers, threads, worker, &p);

    int result = 0;
    const char *message = NULL;
    uint32_t crc = 0, isize = 0;
    size_t k = 0;
    pthread_mutex_lock(&p.lock);
    if (!started) { result = -1; message = "Cannot start decoder threads"; }
    while (!result) {
        if (*cancel) { result = -2; break; }
        if (p.broken) { result = -1; message = "Cannot allocate decoder state"; break; }
        Chunk *c = &p.chunks[k];
        Piece *piece = c->first;
        if (!piece) {
            if (!c->done) { if (!help(&p)) wait_changed(&p); continue; }
            if (c->failed) { result = -1; message = c->error; break; }
            /* Accepted chunks finish, so this would be a bug. */
            if (c->aborted || c->discarded) { result = -1; message = "Internal gzip decoding error"; break; }
            if (c->next == END) {
                /* A chunk stops short only when told to, never so. */
                if (!c->ended) { result = -1; message = "Internal gzip decoding error"; }
                break;
            }
            for (size_t j = k + 1; j < c->next; ++j) discard(&p, j);
            free(c->window);
            c->window = NULL;
            k = c->next;
            p.head = k;
            pthread_cond_broadcast(&p.changed);
            continue;
        }
        if (!piece->ready) { if (!help(&p)) wait_changed(&p); continue; }
        c->first = piece->next;
        if (!c->first) c->last = NULL;
        c->queued--;
        pthread_mutex_unlock(&p.lock);
        size_t n = piece->length;
        crc = n ? (uint32_t)crc32_combine(crc, piece->crc, (z_off_t)n) : crc;
        isize += (uint32_t)n;
        if (n && sink(context, piece->out, n, piece->consumed)) result = -2;
        if (piece->member_end) {
            if (!result && (crc != piece->want_crc || isize != piece->want_size)) {
                result = -1;
                message = "Incorrect gzip checksum or length";
            }
            crc = isize = 0;
        }
        pthread_mutex_lock(&p.lock);
        recycle(&p, piece);
        pthread_cond_broadcast(&p.changed);
    }
    if (message) snprintf(error, error_size, "%s", message);
    p.stop = true;
    pthread_cond_broadcast(&p.changed);
    pthread_mutex_unlock(&p.lock);
    for (unsigned i = 0; i < started; ++i) pthread_join(workers[i], NULL);
    for (size_t i = 0; i < p.count; ++i) {
        free_list(p.chunks[i].first);
        free(p.chunks[i].window);
        free(p.chunks[i].tail);
        free(p.chunks[i].tail_shadow);
    }
    free_list(p.spare);
    free_list(p.spare_marked);
    free(p.chunks);
    free(workers);
    pthread_mutex_destroy(&p.lock);
    pthread_cond_destroy(&p.changed);
    return result;
}
