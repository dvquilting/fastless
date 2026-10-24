/* zstd decoding on many threads, for input of several frames: pzstd and
   zstd --seekable write them, and concatenated files have them. Frames are
   independent, so the input is cut into runs of whole frames, and workers
   decode runs into pieces of output. Once the runs before one are decoded,
   its place in the output is known, and the workers copy its pieces there
   (the main thread helps while it waits), which spreads that copying, and
   faulting in the output's pages, over the threads. The main thread takes
   the pieces in order. Frame checksums are checked by libzstd. A single
   frame, as zstd -T0 writes, cannot be split and decodes on one thread.

   Cutting runs means walking each frame's block headers. The main thread
   walks only the first run before the workers start, and gives up on a
   first frame of over FIRST_FRAME bytes, which would be a long walk for
   input that then decodes on one thread anyway; idle workers walk on from
   there while others decode. Input that does not walk as frames, such as
   padding or a truncated frame, becomes a last run, whose decoding reports
   it as the one-thread decoder would. */
#define _GNU_SOURCE
#include "zframes.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>

/* Runs are at least ZSTD_RUN bytes of input; output goes in pieces of
   ZSTD_PIECE bytes. Test builds make these tiny. */
#ifndef ZSTD_RUN
#define ZSTD_RUN ((size_t)1 << 20)
#endif
#ifndef ZSTD_PIECE
#define ZSTD_PIECE ((size_t)1 << 20)
#endif
#ifndef ZSTD_BUDGET_PER_THREAD
#define ZSTD_BUDGET_PER_THREAD ((size_t)32 << 20)
#endif
/* Decoder windows across threads are kept to about this much memory; a
   worker frees its decoder after windows over BIG_WINDOW. */
#define WINDOW_BUDGET ((uint64_t)1 << 30)
#define BIG_WINDOW ((uint64_t)16 << 20)
#define FIRST_FRAME ((size_t)64 << 20)
/* The run the main thread is reading may hold this many pieces beyond the
   budget, so that it never waits for runs behind it. */
#define HEAD_PIECES 4

typedef struct Piece {
    struct Piece *next, *place_next;
    size_t run, at;           /* its run, and where in that run's output */
    unsigned char *data;
    const unsigned char *out; /* `data`, or where it was placed */
    size_t length, consumed;
    bool ready;               /* placed, or not to be */
} Piece;

typedef struct {
    size_t from, to;          /* input bytes */
    uint64_t window;          /* the widest window its frames ask for */
    bool claimed, done, failed, known;
    size_t offset, produced;  /* where its output goes, once `known` */
    Piece *first, *last;
    size_t queued;            /* pieces waiting for the main thread */
    char error[128];
} Run;

typedef struct {
    const unsigned char *source;
    size_t length;
    Run *runs;                /* room for every run the input can make */
    size_t count, claim, head, frontier;
    size_t walked;            /* input cut into runs so far */
    bool walking, walk_done;
    unsigned threads;
    uint64_t windows;         /* window memory of runs being decoded */
    volatile sig_atomic_t *cancel;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    /* Output held is limited to `budget` bytes, except for the head's first
       HEAD_PIECES. */
    size_t buffered, budget;
    Piece *spare, *place_first, *place_last;
    OrderedPlace place;
    void *context;
    bool stop, broken;
} Pool;

/* The window a frame header asks for, 0 for a skippable frame, or
   UINT64_MAX when it cannot be read. */
static uint64_t frame_window(const unsigned char *s, size_t n) {
    if (n >= 4 && (s[0] & 0xf0) == 0x50 && s[1] == 0x2a && s[2] == 0x4d && s[3] == 0x18) return 0;
    if (n < 6 || s[0] != 0x28 || s[1] != 0xb5 || s[2] != 0x2f || s[3] != 0xfd) return UINT64_MAX;
    unsigned fhd = s[4];
    if (!(fhd & 0x20)) {
        unsigned wd = s[5], log = 10 + (wd >> 3);
        uint64_t base = (uint64_t)1 << log;
        return base + base / 8 * (wd & 7);
    }
    /* A single segment: the window is the content size. */
    static const unsigned char id_bytes[4] = {0, 1, 2, 4}, size_bytes[4] = {1, 2, 4, 8};
    size_t at = 5 + id_bytes[fhd & 3], k = size_bytes[fhd >> 6];
    if (n < at + k) return UINT64_MAX;
    uint64_t size = 0;
    for (size_t i = 0; i < k; ++i) size |= (uint64_t)s[at + i] << (8 * i);
    return k == 2 ? size + 256 : size;
}

static void recycle(Pool *p, Piece *piece) {
    p->buffered -= ZSTD_PIECE;
    piece->next = p->spare;
    p->spare = piece;
}
static bool alive(const Pool *p) { return !p->stop && !*p->cancel; }
/* A piece for run r, or NULL when the worker should stop, or with *failed
   set when there is no memory for one. */
static Piece *take_piece(Pool *p, size_t r, bool *failed) {
    pthread_mutex_lock(&p->lock);
    while (alive(p) && p->buffered + ZSTD_PIECE > p->budget && !(r == p->head && p->runs[r].queued < HEAD_PIECES))
        pthread_cond_wait(&p->changed, &p->lock);
    Piece *piece = NULL;
    if (alive(p)) {
        if (p->spare) { piece = p->spare; p->spare = piece->next; }
        else if ((piece = calloc(1, sizeof(*piece))) && !(piece->data = malloc(ZSTD_PIECE))) {
            free(piece);
            piece = NULL;
        }
        if (!piece) *failed = true;
        if (piece) {
            p->buffered += ZSTD_PIECE;
            unsigned char *data = piece->data;
            *piece = (Piece){.run = r, .data = data};
        }
    }
    pthread_mutex_unlock(&p->lock);
    return piece;
}
static void put(Pool *p, Piece *piece, size_t offset) {
    piece->out = piece->data;
    if (p->place && piece->length) {
        unsigned char *at = p->place(p->context, offset + piece->at, piece->length);
        if (at) {
            memcpy(at, piece->data, piece->length);
            piece->out = at;
        }
    }
}
/* Places one piece waiting for that, if any; called with the lock held,
   which it releases meanwhile. */
static bool help(Pool *p) {
    Piece *piece = p->place_first;
    if (!piece) return false;
    p->place_first = piece->place_next;
    if (!p->place_first) p->place_last = NULL;
    size_t offset = p->runs[piece->run].offset;
    pthread_mutex_unlock(&p->lock);
    put(p, piece, offset);
    pthread_mutex_lock(&p->lock);
    piece->ready = true;
    pthread_cond_broadcast(&p->changed);
    return true;
}
static void give(Pool *p, size_t r, Piece *piece) {
    Run *run = &p->runs[r];
    pthread_mutex_lock(&p->lock);
    if (run->known) {
        size_t offset = run->offset;
        pthread_mutex_unlock(&p->lock);
        put(p, piece, offset);
        pthread_mutex_lock(&p->lock);
        piece->ready = true;
    }
    piece->next = NULL;
    if (run->last) run->last->next = piece;
    else run->first = piece;
    run->last = piece;
    run->queued++;
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
}
/* Works out where runs' output goes, in order, as far as the runs before
   them are decoded; called with the lock held. */
static void advance(Pool *p) {
    while (p->frontier + 1 < p->count) {
        Run *r = &p->runs[p->frontier], *next = r + 1;
        if (!r->done || r->failed) return;
        next->offset = r->offset + r->produced;
        next->known = true;
        for (Piece *piece = next->first; piece; piece = piece->next)
            if (!piece->ready) {
                piece->place_next = NULL;
                if (p->place_last) p->place_last->place_next = piece;
                else p->place_first = piece;
                p->place_last = piece;
            }
        p->frontier++;
        pthread_cond_broadcast(&p->changed);
    }
}
static void run_frames(Pool *p, size_t r, ZSTD_DCtx *z) {
    Run *run = &p->runs[r];
    const char *error = NULL;
    size_t produced = 0, rc = 1;
    ZSTD_inBuffer in = {p->source + run->from, run->to - run->from, 0};
    ZSTD_DCtx_reset(z, ZSTD_reset_session_only);
    Piece *piece = NULL;
    for (;;) {
        if (!piece) {
            bool failed = false;
            if (!(piece = take_piece(p, r, &failed))) {
                if (failed) error = "Cannot allocate decompression buffer";
                break;
            }
            piece->at = produced;
        }
        ZSTD_outBuffer out = {piece->data + piece->length, ZSTD_PIECE - piece->length, 0};
        size_t before = in.pos;
        rc = ZSTD_decompressStream(z, &out, &in);
        if (ZSTD_isError(rc)) { error = ZSTD_getErrorName(rc); break; }
        piece->length += out.pos;
        produced += out.pos;
        piece->consumed = run->from + in.pos;
        bool finished = !rc && in.pos == in.size;
        if (finished || piece->length == ZSTD_PIECE) {
            give(p, r, piece);
            piece = NULL;
        }
        if (finished) break;
        /* Input that ends inside a frame. */
        if (!out.pos && before == in.pos && in.pos == in.size) { error = "Truncated or invalid zstd stream"; break; }
    }
    pthread_mutex_lock(&p->lock);
    if (piece) recycle(p, piece);
    run->produced = produced;
    if (error) {
        run->failed = true;
        snprintf(run->error, sizeof(run->error), "%s", error);
    }
    run->done = true;
    p->windows -= run->window;
    advance(p);
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
}
static void wait_changed(Pool *p) { ordered_wait(&p->changed, &p->lock); }
/* Walks frames from `pos` until they make a run of ZSTD_RUN bytes or the
   input ends, each frame up to `most` bytes, and returns where the run ends:
   the end of the input once the frames stop walking, unless `strict`, when
   it returns 0. */
static size_t walk_run(const unsigned char *s, size_t length, size_t pos, size_t most, bool strict,
                       uint64_t *window, size_t *frames) {
    size_t start = pos;
    *window = 0;
    while (pos < length && pos - start < ZSTD_RUN) {
        size_t left = length - pos;
        size_t n = ZSTD_findFrameCompressedSize(s + pos, left < most ? left : most);
        uint64_t w = frame_window(s + pos, left);
        if (ZSTD_isError(n) || w == UINT64_MAX) {
            if (strict) return 0;
            *window = WINDOW_BUDGET; /* decoded alone, to be safe */
            return length;
        }
        if (w) ++*frames;
        if (w > *window) *window = w;
        pos += n;
    }
    return pos;
}
static void *worker(void *arg) {
    Pool *p = arg;
    ZSTD_DCtx *z = ZSTD_createDCtx();
    pthread_mutex_lock(&p->lock);
    if (!z) {
        /* Otherwise the runs it would have taken might never be. */
        p->broken = true;
        pthread_cond_broadcast(&p->changed);
    }
    while (z && alive(p)) {
        if (help(p)) continue;
        Run *next = p->claim < p->count ? &p->runs[p->claim] : NULL;
        if (next && (!p->windows || p->windows + next->window <= WINDOW_BUDGET)) {
            size_t r = p->claim++;
            next->claimed = true;
            p->windows += next->window;
            pthread_mutex_unlock(&p->lock);
            run_frames(p, r, z);
            if (next->window > BIG_WINDOW) {
                /* Give back what the decoder kept for it. */
                ZSTD_freeDCtx(z);
                z = ZSTD_createDCtx();
            }
            pthread_mutex_lock(&p->lock);
            if (!z) {
                p->broken = true;
                pthread_cond_broadcast(&p->changed);
            }
            continue;
        }
        /* One worker at a time cuts more runs when few are left. */
        if (!p->walk_done && !p->walking && p->count - p->claim < 2 * (size_t)p->threads) {
            size_t from = p->walked, frames = 0;
            uint64_t window;
            p->walking = true;
            pthread_mutex_unlock(&p->lock);
            size_t to = walk_run(p->source, p->length, from, p->length, false, &window, &frames);
            pthread_mutex_lock(&p->lock);
            p->runs[p->count++] = (Run){.from = from, .to = to, .window = window};
            p->walked = to;
            p->walk_done = to == p->length;
            p->walking = false;
            advance(p);
            pthread_cond_broadcast(&p->changed);
            continue;
        }
        wait_changed(p);
    }
    pthread_mutex_unlock(&p->lock);
    ZSTD_freeDCtx(z);
    return NULL;
}

int zstd_frames_decode(const unsigned char *source, size_t length, unsigned threads,
                       volatile sig_atomic_t *cancel, OrderedSink sink, OrderedPlace place, void *context,
                       char *error, size_t error_size) {
    if (threads < 2 || length < 2 * ZSTD_RUN) return 1;
    /* The first run, which must walk, and must not be all the input. */
    size_t frames = 0;
    uint64_t window;
    size_t first = walk_run(source, length, 0, FIRST_FRAME, true, &window, &frames);
    if (!first || first == length) return 1;
    /* Every run but the last holds at least ZSTD_RUN bytes. */
    Run *runs = calloc(length / ZSTD_RUN + 2, sizeof(*runs));
    pthread_t *workers = calloc(threads, sizeof(*workers));
    if (!runs || !workers) { free(runs); free(workers); return 1; }
    runs[0] = (Run){.from = 0, .to = first, .window = window, .known = true};
    Pool p = {.source = source, .length = length, .runs = runs, .count = 1, .walked = first,
              .threads = threads, .cancel = cancel, .budget = (size_t)threads * ZSTD_BUDGET_PER_THREAD,
              .place = place, .context = context};
    pthread_mutex_init(&p.lock, NULL);
    pthread_cond_init(&p.changed, NULL);
    unsigned started = ordered_start(workers, threads, worker, &p);

    int result = 0;
    const char *message = NULL;
    size_t k = 0;
    pthread_mutex_lock(&p.lock);
    if (!started) { result = -1; message = "Cannot start decoder threads"; }
    while (!result) {
        if (*cancel) { result = -2; break; }
        if (p.broken) { result = -1; message = "Cannot allocate decoder state"; break; }
        Run *r = &p.runs[k];
        Piece *piece = r->first;
        if (!piece || !piece->ready) {
            if (!piece && r->done && k + 1 < p.count) {
                if (r->failed) { result = -1; message = r->error; break; }
                p.head = ++k;
                pthread_cond_broadcast(&p.changed);
                continue;
            }
            if (!piece && r->done && (r->failed || p.walk_done)) {
                if (r->failed) { result = -1; message = r->error; }
                break;
            }
            if (!help(&p)) wait_changed(&p);
            continue;
        }
        r->first = piece->next;
        if (!r->first) r->last = NULL;
        r->queued--;
        pthread_mutex_unlock(&p.lock);
        if (piece->length && sink(context, piece->out, piece->length, piece->consumed)) result = -2;
        pthread_mutex_lock(&p.lock);
        recycle(&p, piece);
        pthread_cond_broadcast(&p.changed);
    }
    if (message) snprintf(error, error_size, "%s", message);
    p.stop = true;
    pthread_cond_broadcast(&p.changed);
    pthread_mutex_unlock(&p.lock);
    for (unsigned i = 0; i < started; ++i) pthread_join(workers[i], NULL);
    for (size_t i = 0; i < p.count; ++i)
        for (Piece *piece = p.runs[i].first, *next; piece; piece = next) {
            next = piece->next;
            free(piece->data);
            free(piece);
        }
    for (Piece *piece = p.spare, *next; piece; piece = next) {
        next = piece->next;
        free(piece->data);
        free(piece);
    }
    free(p.runs);
    free(workers);
    pthread_mutex_destroy(&p.lock);
    pthread_cond_destroy(&p.changed);
    return result;
}
