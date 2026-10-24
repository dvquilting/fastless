/* bzip2 decoding on many threads with lbzip2's block decoder.

   The input is mapped whole, so instead of lbzip2's streaming scheduler a
   simpler scheme works. Workers scan ahead for the 48-bit pattern that
   starts every block and decode each block found, speculatively. The main
   thread follows the stream in order with lbzip2's parser, which validates
   headers and the stream CRC: each block starts where the previous one
   ended, and only the speculative result for exactly that position is used,
   after its block CRC is checked. Patterns found inside compressed data
   (rare) are decoded and discarded; a block the scan missed is decoded by
   the main thread. */
#define _GNU_SOURCE
#include "bzip2.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "decode.h"

/* lbzip2's decoder allocates with this; out of memory is fatal there too. */
void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) { fputs("fastless: out of memory decoding bzip2\n", stderr); exit(2); }
    return p;
}

/* Blocks are emitted in pieces of EMIT_CHUNK bytes, and the input is scanned
   SCAN_STEP bits at a time. Test builds make these tiny. */
#ifndef EMIT_CHUNK
#define EMIT_CHUNK ((size_t)1 << 20)
#endif
#ifndef SCAN_STEP
#define SCAN_STEP ((uint64_t)32 << 20)
#endif
#ifndef BUDGET_PER_THREAD
#define BUDGET_PER_THREAD ((size_t)16 << 20)
#endif
#define HEADER_BITS 80                /* block magic and CRC */
#define ERR_TRAILING (ERR_EOF + 1)

static const char *bz_error(int rc) {
    switch (rc) {
    case ERR_MAGIC: return "bad stream header";
    case ERR_HEADER: return "bad block header";
    case ERR_BLKCRC: return "block CRC mismatch";
    case ERR_STRMCRC: return "stream CRC mismatch";
    case ERR_OVERFLOW: return "block larger than its stream allows";
    case ERR_EOF: return "Truncated input";
    case ERR_TRAILING: return "Unexpected data after the last stream";
    default: return "invalid compressed data";
    }
}

/* The input as 32-bit big-endian words, as lbzip2 reads it. A last partial
   word is read from a zero-padded copy, never past the end of the input. */
typedef struct {
    const uint32_t *words;
    size_t full;
    uint32_t tail;
    bool has_tail;
    uint64_t bits;
} Source;

static struct bitstream bits_at(const Source *s, uint64_t pos, bool *on_tail) {
    struct bitstream bs = {0};
    size_t w = (size_t)(pos / 32);
    if (w < s->full) {
        bs.data = s->words + w;
        bs.limit = s->words + s->full;
        *on_tail = false;
    } else {
        bs.limit = &s->tail + (s->has_tail ? 1 : 0);
        bs.data = s->has_tail && w == s->full ? &s->tail : bs.limit;
        bs.eof = true;
        *on_tail = true;
    }
    unsigned skip = (unsigned)(pos % 32);
    if (skip && bs.data < bs.limit) {
        bs.buff = (uint64_t)ntohl(*bs.data++) << 32 << skip;
        bs.live = 32 - skip;
    }
    return bs;
}
static void to_tail(const Source *s, struct bitstream *bs, bool *on_tail) {
    bs->data = &s->tail;
    bs->limit = &s->tail + (s->has_tail ? 1 : 0);
    bs->eof = true;
    *on_tail = true;
}
static uint64_t bits_pos(const Source *s, const struct bitstream *bs, bool on_tail) {
    uint64_t word = on_tail ? s->full + (uint64_t)(bs->data - &s->tail) : (uint64_t)(bs->data - s->words);
    return word * 32 - bs->live;
}

/* One block's decoding, done by a worker or by the main thread. */
typedef struct {
    enum { EMPTY, RUNNING, DONE } state;
    uint64_t start, end;     /* bit positions of the block body and its end */
    int status;
    unsigned char **chunks;  /* the output, EMIT_CHUNK bytes each but the last */
    size_t chunk_count, length;
    uint32_t crc, block_size;
} Block;

typedef struct {
    Source src;
    unsigned threads;
    volatile sig_atomic_t *cancel;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    /* Candidate block starts in input order: global index i is at
       starts[i - first]. Those before `consumed` are finished with. */
    uint64_t *starts;
    size_t first, count, cap, next, consumed;
    Block *slots;            /* candidate i decodes into slots[i % window] */
    size_t window;
    uint64_t scanned;        /* bits searched for block starts so far */
    bool scanning, stop;
    /* Output pieces and decoding tables are reused, which saves faulting
       in fresh pages for every block. Output held is limited to `budget`
       bytes, except for the next block wanted, which always proceeds. */
    unsigned char **spare_chunks;
    uint32_t **spare_tables;
    size_t spare_chunk_count, spare_table_count, buffered, budget;
} Pool;

static bool push(void ***items, size_t *count, void *item) {
    void **grown = realloc(*items, (*count + 1) * sizeof(void *));
    if (!grown) return false;
    *items = grown;
    grown[(*count)++] = item;
    return true;
}
/* An output piece for candidate `index` (SIZE_MAX: decoded by the main
   thread), or NULL when decoding stops. Called with the lock held. */
static unsigned char *take_chunk(Pool *p, size_t index) {
    while (p->buffered + EMIT_CHUNK > p->budget && index != SIZE_MAX && index != p->consumed &&
           !p->stop && !*p->cancel)
        pthread_cond_wait(&p->changed, &p->lock);
    if (p->stop || *p->cancel) return NULL;
    p->buffered += EMIT_CHUNK;
    return p->spare_chunk_count ? p->spare_chunks[--p->spare_chunk_count] : xmalloc(EMIT_CHUNK);
}
static void give_chunks(Pool *p, Block *b) {
    for (size_t i = 0; i < b->chunk_count; ++i) {
        if (!push((void ***)&p->spare_chunks, &p->spare_chunk_count, b->chunks[i])) free(b->chunks[i]);
        p->buffered -= EMIT_CHUNK;
    }
    free(b->chunks);
    *b = (Block){0};
    pthread_cond_broadcast(&p->changed);
}
static void run_block(Pool *p, Block *b, size_t index) {
    struct decoder_state ds;
    decoder_init(&ds);
    pthread_mutex_lock(&p->lock);
    if (p->spare_table_count) { free(ds.tt); ds.tt = p->spare_tables[--p->spare_table_count]; }
    pthread_mutex_unlock(&p->lock);
    bool tail;
    struct bitstream bs = bits_at(&p->src, b->start, &tail);
    b->status = retrieve(&ds, &bs);
    if (b->status == MORE && !tail) {
        to_tail(&p->src, &bs, &tail);
        b->status = retrieve(&ds, &bs);
    }
    b->end = bits_pos(&p->src, &bs, tail);
    if (b->status == OK) {
        b->block_size = ds.block_size;
        decode(&ds);
        for (;;) {
            pthread_mutex_lock(&p->lock);
            unsigned char *chunk = take_chunk(p, index);
            bool kept = chunk && push((void ***)&b->chunks, &b->chunk_count, chunk);
            if (chunk && !kept) { free(chunk); p->buffered -= EMIT_CHUNK; }
            pthread_mutex_unlock(&p->lock);
            if (!kept) { b->status = MORE; break; } /* stopping */
            size_t room = EMIT_CHUNK;
            int rc = emit(&ds, chunk, &room);
            b->length += EMIT_CHUNK - room;
            if (rc == MORE) continue;
            b->status = rc;
            b->crc = ds.crc;
            break;
        }
    }
    free(ds.internal_state);
    pthread_mutex_lock(&p->lock);
    if (!push((void ***)&p->spare_tables, &p->spare_table_count, ds.tt)) free(ds.tt);
    pthread_mutex_unlock(&p->lock);
}

static bool add_start(uint64_t **found, size_t *count, size_t *cap, uint64_t body) {
    if (*count == *cap) {
        size_t grown_cap = *cap ? *cap * 2 : 64;
        uint64_t *grown = realloc(*found, grown_cap * sizeof(*grown));
        if (!grown) return false; /* The main thread decodes what is not found. */
        *found = grown;
        *cap = grown_cap;
    }
    (*found)[(*count)++] = body;
    return true;
}
/* Finds block starts whose header begins in [from, to). */
static void scan_range(const Source *s, uint64_t from, uint64_t to, uint64_t **found, size_t *count, size_t *cap) {
    bool tail;
    struct bitstream bs = bits_at(s, from, &tail);
    if (tail) return;
    size_t limit = (size_t)((to + HEADER_BITS) / 32) + 2;
    if (limit < s->full) bs.limit = s->words + limit;
#ifdef BZIP2_SCAN_TEST
    /* Counted across ranges, which are too short to hold three blocks; one
       thread scans at a time. */
    static size_t seen;
#endif
    while (scan(&bs, 0) == OK) {
        uint64_t body = bits_pos(s, &bs, false), header = body - HEADER_BITS;
        if (header >= to) break;
        if (header < from) continue;
#ifdef BZIP2_SCAN_TEST
        /* Test builds miss every third block and find a false start before
           each other one, as a scan can, so that the main thread's decoding
           and discarded work are exercised. */
        if (++seen % 3 == 0) continue;
        if (!add_start(found, count, cap, body - 1)) break;
#endif
        if (!add_start(found, count, cap, body)) break;
    }
}
static void append_starts(Pool *p, const uint64_t *found, size_t n) {
    size_t done = p->consumed - p->first;
    if (done > 4096 && done * 2 > p->count - p->first) {
        memmove(p->starts, p->starts + done, (p->count - p->consumed) * sizeof(*p->starts));
        p->first = p->consumed;
    }
    size_t held = p->count - p->first;
    if (held + n > p->cap) {
        size_t cap = (held + n) * 2;
        uint64_t *grown = realloc(p->starts, cap * sizeof(*grown));
        if (!grown) return;
        p->starts = grown;
        p->cap = cap;
    }
    memcpy(p->starts + held, found, n * sizeof(*found));
    p->count += n;
}
static void *worker(void *arg) {
    Pool *p = arg;
    pthread_mutex_lock(&p->lock);
    while (!p->stop && !*p->cancel) {
        if (p->next < p->count && p->next < p->consumed + p->window) {
            size_t i = p->next++;
            Block b = {.start = p->starts[i - p->first]};
            p->slots[i % p->window].state = RUNNING;
            pthread_mutex_unlock(&p->lock);
            run_block(p, &b, i);
            pthread_mutex_lock(&p->lock);
            b.state = DONE;
            p->slots[i % p->window] = b;
            pthread_cond_broadcast(&p->changed);
            continue;
        }
        /* One worker at a time scans ahead, in order, when work runs low. */
        if (!p->scanning && p->scanned < p->src.bits && p->count - p->next < 2 * (size_t)p->threads) {
            uint64_t from = p->scanned, to = p->src.bits - from > SCAN_STEP ? from + SCAN_STEP : p->src.bits;
            p->scanning = true;
            pthread_mutex_unlock(&p->lock);
            uint64_t *found = NULL;
            size_t n = 0, cap = 0;
            scan_range(&p->src, from, to, &found, &n, &cap);
            pthread_mutex_lock(&p->lock);
            append_starts(p, found, n);
            free(found);
            p->scanned = to;
            p->scanning = false;
            pthread_cond_broadcast(&p->changed);
            continue;
        }
        if (p->scanned >= p->src.bits && p->next >= p->count) break;
        pthread_cond_wait(&p->changed, &p->lock);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}
static void wait_changed(Pool *p) { ordered_wait(&p->changed, &p->lock); }
/* The decoded block starting at `pos`, from the workers if they found it:
   false only when cancelled. */
static bool block_at(Pool *p, uint64_t pos, Block *out) {
    pthread_mutex_lock(&p->lock);
    for (;;) {
        if (*p->cancel) { pthread_mutex_unlock(&p->lock); return false; }
        size_t i = p->consumed;
        if (i < p->count && p->starts[i - p->first] <= pos) {
            /* A worker is or will be decoding candidate i. */
            Block *slot = &p->slots[i % p->window];
            if (i >= p->next || slot->state != DONE) { wait_changed(p); continue; }
            bool match = p->starts[i - p->first] == pos;
            if (match) { *out = *slot; *slot = (Block){0}; }
            else give_chunks(p, slot); /* A pattern inside compressed data. */
            p->consumed++;
            pthread_cond_broadcast(&p->changed);
            if (match) { pthread_mutex_unlock(&p->lock); return true; }
            continue;
        }
        /* Not found where the scan has been: decode it here. */
        if (p->scanned >= pos - HEADER_BITS + 1 || p->scanned >= p->src.bits) break;
        wait_changed(p);
    }
    pthread_mutex_unlock(&p->lock);
    *out = (Block){.start = pos};
    run_block(p, out, SIZE_MAX);
    return true;
}

int bzip2_decode(const unsigned char *source, size_t length, unsigned threads,
                 volatile sig_atomic_t *cancel, OrderedSink sink, void *context,
                 char *error, size_t error_size) {
    if (length < 4 || memcmp(source, "BZh", 3) || source[3] < '1' || source[3] > '9') {
        snprintf(error, error_size, "%s", bz_error(ERR_MAGIC));
        return -1;
    }
    if (!threads) threads = 1;
    Pool p = {.src = {.words = (const uint32_t *)(const void *)source, .full = length / 4, .bits = (uint64_t)length * 8},
              .threads = threads, .cancel = cancel, .window = threads + threads / 2 + 2};
    /* Typical blocks emit about 1 MiB and never reach this; long runs of one
       byte emit up to 45 MiB a block, where it keeps several in progress,
       about as many as lbzip2 does in about as much memory. */
    p.budget = (size_t)threads * BUDGET_PER_THREAD;
    if (length % 4) {
        memcpy(&p.src.tail, source + length / 4 * 4, length % 4);
        p.src.has_tail = true;
    }
    p.slots = calloc(p.window, sizeof(*p.slots));
    pthread_t *workers = calloc(threads, sizeof(*workers));
    unsigned started = 0;
    if (!p.slots || !workers) {
        free(p.slots); free(workers);
        snprintf(error, error_size, "Cannot allocate decoder state");
        return -1;
    }
    pthread_mutex_init(&p.lock, NULL);
    pthread_cond_init(&p.changed, NULL);
    started = ordered_start(workers, threads, worker, &p);

    struct parser_state ps;
    parser_init(&ps, source[3] - '0', 0);
    uint64_t pos = 32;
    int result = 0, rc = OK;
    for (;;) {
        if (*cancel) { result = -2; break; }
        struct header hd;
        unsigned garbage = 0;
        bool tail;
        struct bitstream bs = bits_at(&p.src, pos, &tail);
        rc = parse(&ps, &hd, &bs, &garbage);
        if (rc == MORE && !tail) {
            to_tail(&p.src, &bs, &tail);
            rc = parse(&ps, &hd, &bs, &garbage);
        }
        pos = bits_pos(&p.src, &bs, tail);
        if (rc == FINISH) {
            /* As for other formats, only zero padding may follow the last
               stream, which must not itself run into the end. */
            uint64_t end = pos - garbage;
            rc = end > p.src.bits ? ERR_EOF : OK;
            for (size_t i = (size_t)(end / 8); rc == OK && i < length; ++i)
                if (source[i]) rc = ERR_TRAILING;
            break;
        }
        /* Errors that came from reading past the end mean the input ends early. */
        if (rc != OK) { if (pos > p.src.bits) rc = ERR_EOF; break; }
        Block b;
        if (!started) { b = (Block){.start = pos}; run_block(&p, &b, SIZE_MAX); }
        else if (!block_at(&p, pos, &b)) { result = -2; break; }
        /* A block cut short by cancellation is not damage. */
        if (*cancel) result = -2;
        rc = b.end > p.src.bits ? ERR_EOF : b.status;
        if (rc == OK && b.block_size > (uint32_t)hd.bs100k * 100000u) rc = ERR_OVERFLOW;
        if (rc == OK && b.crc != hd.crc) rc = ERR_BLKCRC;
        size_t consumed = (size_t)(b.end / 8);
        for (size_t i = 0, left = b.length; rc == OK && !result && left; ++i) {
            size_t n = left < EMIT_CHUNK ? left : EMIT_CHUNK;
            if (sink(context, b.chunks[i], n, consumed)) result = -2;
            left -= n;
        }
        pos = b.end;
        pthread_mutex_lock(&p.lock);
        give_chunks(&p, &b);
        pthread_mutex_unlock(&p.lock);
        if (rc != OK || result) break;
    }
    pthread_mutex_lock(&p.lock);
    p.stop = true;
    pthread_cond_broadcast(&p.changed);
    pthread_mutex_unlock(&p.lock);
    for (unsigned i = 0; i < started; ++i) pthread_join(workers[i], NULL);
    for (size_t i = 0; i < p.window; ++i)
        if (p.slots[i].state == DONE) give_chunks(&p, &p.slots[i]);
    for (size_t i = 0; i < p.spare_chunk_count; ++i) free(p.spare_chunks[i]);
    for (size_t i = 0; i < p.spare_table_count; ++i) free(p.spare_tables[i]);
    free(p.spare_chunks);
    free(p.spare_tables);
    free(p.slots);
    free(p.starts);
    free(workers);
    pthread_mutex_destroy(&p.lock);
    pthread_cond_destroy(&p.changed);
    if (result) return result;
    if (rc != OK) { snprintf(error, error_size, "%s", bz_error(rc)); return -1; }
    return 0;
}
