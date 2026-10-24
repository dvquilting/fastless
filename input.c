#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _FILE_OFFSET_BITS 64
#include "input.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#ifdef FASTLESS_ISAL
#include <isa-l/igzip_lib.h>
#else
#include <zlib.h>
#endif
#include <zstd.h>
#include <lzma.h>
#ifdef FASTLESS_LBZIP2
#include "bzip2.h"
#else
#include <bzlib.h>
#endif

#ifndef BUFFER_SIZE
#define BUFFER_SIZE (8u * 1024u * 1024u)
#endif
#define MIB UINT64_C(1048576)
typedef struct {
    int fd;
    size_t written, source_size;
    /* Decoder output goes to `buffer`, which holds `capacity` bytes. Until
       the output exceeds `limit` it is decoded in place into `memory`, a
       reservation of `reserved` bytes whose first `committed` are writable;
       after that it spills to a temporary file in `temp_dir`. */
    unsigned char *buffer, *memory;
    size_t capacity, limit, reserved, committed;
    bool in_place;
    const char *temp_dir;
    volatile sig_atomic_t *cancel;
    char *error;
    size_t error_size;
    const char *format;
    bool progress, stream; /* stream: copying a pipe or device, not decoding */
    struct timespec last_update;
    /* Writer thread: decoding continues into `buffer` while `pending` is
       written out. Everything below is guarded by `lock`. */
    bool threaded, stop;
    pthread_t writer;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned char *pending, *spare;
    size_t pending_length;
    int write_error; /* errno of a failed write, or -1 for a zero-length one */
} Decoder;

static int failure(Decoder *d, const char *message) {
    snprintf(d->error, d->error_size, "%s: %s", d->format, message);
    return -1;
}
static bool cancelled(Decoder *d) {
    if (!*d->cancel) return false;
    failure(d, d->stream ? "Reading interrupted" : "Decompression interrupted");
    return true;
}
static int write_failure(Decoder *d, int error) {
    char message[192];
    snprintf(message, sizeof(message), "Temporary-file write failed: %s",
             error > 0 ? strerror(error) : "zero-length write");
    return failure(d, message);
}
/* Returns 0, or the failure code described at write_error. Stops quietly on
   cancellation, which the caller reports. */
static int write_fd(int fd, const unsigned char *buffer, size_t length, volatile sig_atomic_t *cancel) {
    size_t pos = 0;
    while (pos < length && !*cancel) {
        /* macOS rejects writes over INT_MAX bytes. */
        size_t piece = length - pos < (1u << 30) ? length - pos : (1u << 30);
        ssize_t n = write(fd, buffer + pos, piece);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? errno : -1;
        pos += (size_t)n;
    }
    return 0;
}
static int write_all(Decoder *d, const unsigned char *buffer, size_t length) {
    return write_fd(d->fd, buffer, length, d->cancel);
}
static void *writer_main(void *arg) {
    Decoder *d = arg;
    pthread_mutex_lock(&d->lock);
    for (;;) {
        while (!d->pending && !d->stop) pthread_cond_wait(&d->changed, &d->lock);
        if (!d->pending) break;
        unsigned char *buffer = d->pending;
        size_t length = d->pending_length;
        pthread_mutex_unlock(&d->lock);
        int error = write_all(d, buffer, length);
        pthread_mutex_lock(&d->lock);
        if (error && !d->write_error) d->write_error = error;
        d->spare = buffer;
        d->pending = NULL;
        pthread_cond_broadcast(&d->changed);
    }
    pthread_mutex_unlock(&d->lock);
    return NULL;
}
/* Waits for the writer to finish what it holds and returns its status. */
static int writer_idle(Decoder *d) {
    while (d->pending) pthread_cond_wait(&d->changed, &d->lock);
    return d->write_error;
}
/* In-place output is committed in aligned steps that a transparent huge page
   can fill, which saves most page faults. */
#define COMMIT_STEP ((size_t)2 << 20)
static size_t round_up(size_t n, size_t unit) { return n + (unit - n % unit) % unit; }
static size_t page_round(size_t n) { return round_up(n, (size_t)sysconf(_SC_PAGESIZE)); }
/* Points `buffer` at the next output space: in place while the memory limit
   leaves room and the pages can be committed, otherwise a heap buffer. */
static int next_buffer(Decoder *d) {
    if (d->in_place) {
        size_t room = d->limit - d->written;
        size_t capacity = room < BUFFER_SIZE ? room : BUFFER_SIZE;
        size_t end = round_up(d->written + capacity, COMMIT_STEP);
        if (end > d->reserved) end = d->reserved;
        if (capacity && end > d->committed &&
            mprotect(d->memory + d->committed, end - d->committed, PROT_READ | PROT_WRITE))
            capacity = 0; /* Out of committable memory: spill early. */
        else if (end > d->committed) d->committed = end;
        if (capacity) {
            d->buffer = d->memory + d->written;
            d->capacity = capacity;
            return 0;
        }
        d->in_place = false;
        d->buffer = NULL;
    }
    if (!d->buffer && !(d->buffer = malloc(BUFFER_SIZE)))
        return failure(d, "Cannot allocate decompression buffer");
    d->capacity = BUFFER_SIZE;
    return 0;
}
/* An unlinked temporary file in temp_dir, or -1 with a message. */
static int make_temporary(const char *temp_dir, char *error, size_t error_size) {
    const char suffix[] = "/fastless-XXXXXX";
    size_t dirlen = strlen(temp_dir);
    char *name = dirlen <= SIZE_MAX - sizeof(suffix) ? malloc(dirlen + sizeof(suffix)) : NULL;
    if (!name) { snprintf(error, error_size, "Cannot allocate temporary-file name"); return -1; }
    memcpy(name, temp_dir, dirlen);
    memcpy(name + dirlen, suffix, sizeof(suffix));
    int fd = mkstemp(name);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot create temporary file in %s: %s", temp_dir, strerror(errno));
        free(name); return -1;
    }
    /* No pathname survives cancellation, a crash, or successful mapping. */
    if (unlink(name)) {
        snprintf(error, error_size, "Cannot unlink temporary file: %s", strerror(errno));
        close(fd);
        unlink(name); free(name); return -1;
    }
    free(name);
    return fd;
}
static bool create_temporary(Decoder *d) {
    char message[384];
    d->fd = make_temporary(d->temp_dir, message, sizeof(message));
    if (d->fd < 0) snprintf(d->error, d->error_size, "%s", message);
    return d->fd >= 0;
}
/* Output has outgrown memory: moves what was decoded in place to a temporary
   file, which receives everything from now on. */
static int spill(Decoder *d) {
    if (!create_temporary(d)) return -1;
    if (d->progress && d->written) {
        fprintf(stderr, "\r%s %s: moving %.2f GiB to a temporary file\033[K",
                d->stream ? "Reading" : "Decompressing", d->format, (double)d->written / (1024.0 * 1024.0 * 1024.0));
        fflush(stderr);
    }
    int error = write_all(d, d->memory, d->written);
    if (error) return write_failure(d, error);
    if (cancelled(d)) return -1;
    if (d->memory) munmap(d->memory, d->reserved);
    d->memory = NULL;
    /* Without a second buffer or a thread, writes simply stay on this thread. */
    d->spare = malloc(BUFFER_SIZE);
    d->threaded = d->spare && !pthread_create(&d->writer, NULL, writer_main, d);
    return 0;
}
static int write_output(Decoder *d, size_t length, size_t consumed) {
    if (length > SIZE_MAX - d->written ||
        (uintmax_t)d->written + length > INT64_MAX)
        return failure(d, "Decompressed file exceeds address/file size limits");
    if (cancelled(d)) return -1;
    int error = 0;
    if (d->in_place || !length) {
        /* Already in memory, or nothing to write. */
    } else if (d->fd < 0 && spill(d)) {
        return -1;
    } else if (!d->threaded) error = write_all(d, d->buffer, length);
    else {
        /* Hand the full buffer to the writer and decode on into the other. */
        pthread_mutex_lock(&d->lock);
        error = writer_idle(d);
        if (!error) {
            d->pending = d->buffer;
            d->pending_length = length;
            d->buffer = d->spare;
            pthread_cond_broadcast(&d->changed);
        }
        pthread_mutex_unlock(&d->lock);
    }
    if (error) return write_failure(d, error);
    if (cancelled(d)) return -1;
    d->written += length;
    if (d->progress) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - d->last_update.tv_sec) +
                         (double)(now.tv_nsec - d->last_update.tv_nsec) / 1e9;
        if (elapsed >= 0.25) {
            if (d->stream)
                fprintf(stderr, "\rReading %s: %.2f GiB\033[K", d->format,
                        (double)d->written / (1024.0 * 1024.0 * 1024.0));
            else
                fprintf(stderr, "\rDecompressing %s: %.1f%% input, %.2f GiB output\033[K",
                        d->format, d->source_size ? 100.0 * (double)consumed / (double)d->source_size : 100.0,
                        (double)d->written / (1024.0 * 1024.0 * 1024.0));
            fflush(stderr);
            d->last_update = now;
        }
    }
    return d->fd < 0 ? next_buffer(d) : 0;
}
static size_t chunk_size(size_t remaining) {
    return remaining > BUFFER_SIZE ? BUFFER_SIZE : remaining;
}
#ifdef FASTLESS_ISAL
static const char *isal_error(int rc) {
    switch (rc) {
    case ISAL_INVALID_BLOCK: return "Invalid deflate block";
    case ISAL_INVALID_SYMBOL: return "Invalid deflate symbol";
    case ISAL_INVALID_LOOKBACK: return "Invalid deflate distance";
    case ISAL_INVALID_WRAPPER: return "Invalid gzip header";
    case ISAL_UNSUPPORTED_METHOD: return "Unsupported gzip compression method";
    case ISAL_INCORRECT_CHECKSUM: return "Incorrect gzip checksum or length";
    default: return "Invalid gzip stream";
    }
}
/* ISA-L's igzip decoder, which checks the header, CRC-32, and length of
   each member like zlib. */
static int decode_gzip(Decoder *d, const unsigned char *source, size_t length) {
    struct inflate_state *s = malloc(sizeof(*s));
    if (!s) return failure(d, "Cannot allocate gzip decoder");
    isal_inflate_init(s);
    s->crc_flag = ISAL_GZIP;
    size_t pos = 0;
    int result = -1;
    for (;;) {
        if (cancelled(d)) break;
        if (!s->avail_in) {
            size_t n = chunk_size(length - pos);
            s->next_in = (uint8_t *)(source + pos);
            s->avail_in = (uint32_t)n;
            pos += n;
        }
        uint32_t before = s->avail_in;
        s->next_out = d->buffer;
        s->avail_out = (uint32_t)d->capacity;
        int rc = isal_inflate(s);
        size_t produced = d->capacity - s->avail_out;
        if (write_output(d, produced, pos - s->avail_in)) break;
        if (rc != ISAL_DECOMP_OK) { failure(d, isal_error(rc)); break; }
        if (s->block_state == ISAL_BLOCK_FINISH) {
            size_t consumed = pos - s->avail_in;
            if (consumed == length) { result = 0; break; }
            /* GNU gzip permits zero padding after the last member. */
            if (!source[consumed]) {
                size_t p = consumed;
                while (p < length && !source[p]) {
                    if (!(p & 65535) && cancelled(d)) goto done;
                    ++p;
                }
                if (p == length) { result = 0; break; }
                failure(d, "Invalid trailing data after gzip member"); break;
            }
            uint8_t *next = s->next_in;
            uint32_t available = s->avail_in;
            isal_inflate_reset(s);
            s->crc_flag = ISAL_GZIP;
            s->next_in = next; s->avail_in = available;
        } else if (!produced && before == s->avail_in && pos == length) {
            failure(d, "Truncated or invalid gzip stream"); break;
        }
    }
done:
    free(s);
    return result;
}
#else
static int decode_gzip(Decoder *d, const unsigned char *source, size_t length) {
    z_stream z = {0};
    if (inflateInit2(&z, 15 + 16) != Z_OK) return failure(d, "Cannot initialize zlib");
    size_t pos = 0;
    int result = -1;
    for (;;) {
        if (cancelled(d)) break;
        if (!z.avail_in) {
            size_t n = chunk_size(length - pos);
            z.next_in = (Bytef *)(source + pos);
            z.avail_in = (uInt)n;
            pos += n;
        }
        uInt before = z.avail_in;
        z.next_out = d->buffer;
        z.avail_out = (uInt)d->capacity;
        int rc = inflate(&z, Z_NO_FLUSH);
        size_t produced = d->capacity - z.avail_out;
        if (write_output(d, produced, pos - z.avail_in)) break;
        if (rc == Z_STREAM_END) {
            size_t consumed = pos - z.avail_in;
            if (consumed == length) { result = 0; break; }
            /* GNU gzip permits zero padding after the last member. */
            if (!source[consumed]) {
                size_t p = consumed;
                while (p < length && !source[p]) {
                    if (!(p & 65535) && cancelled(d)) goto done;
                    ++p;
                }
                if (p == length) { result = 0; break; }
                failure(d, "Invalid trailing data after gzip member"); break;
            }
            Bytef *next = z.next_in;
            uInt available = z.avail_in;
            if (inflateReset2(&z, 15 + 16) != Z_OK) { failure(d, "Cannot reset zlib"); break; }
            z.next_in = next; z.avail_in = available;
        } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
            failure(d, z.msg ? z.msg : "Invalid gzip stream"); break;
        } else if (!produced && before == z.avail_in) {
            failure(d, "Truncated or invalid gzip stream"); break;
        }
    }
done:
    inflateEnd(&z);
    return result;
}
#endif
#ifdef FASTLESS_LBZIP2
/* Blocks decoded on lbzip2's threads arrive in order and are copied into the
   output buffers, a full buffer at a time. */
typedef struct { Decoder *d; size_t fill; } Bzip2Output;
static int bzip2_output(void *context, const unsigned char *data, size_t length, size_t consumed) {
    Bzip2Output *o = context;
    while (length) {
        size_t n = o->d->capacity - o->fill < length ? o->d->capacity - o->fill : length;
        memcpy(o->d->buffer + o->fill, data, n);
        o->fill += n;
        data += n;
        length -= n;
        if (o->fill == o->d->capacity) {
            if (write_output(o->d, o->fill, consumed)) return 1;
            o->fill = 0;
        }
    }
    return 0;
}
static int decode_bzip2(Decoder *d, const unsigned char *source, size_t length, unsigned threads) {
    Bzip2Output o = {d, 0};
    char message[256];
    int rc = bzip2_decode(source, length, threads, d->cancel, bzip2_output, &o, message, sizeof(message));
    if (rc == -1) return failure(d, message);
    if (rc) { cancelled(d); return -1; } /* Otherwise the output already failed. */
    return o.fill ? write_output(d, o.fill, length) : 0;
}
#else
static int decode_bzip2(Decoder *d, const unsigned char *source, size_t length, unsigned threads) {
    (void)threads;
    bz_stream z;
    memset(&z, 0, sizeof(z));
    if (BZ2_bzDecompressInit(&z, 0, 0) != BZ_OK) return failure(d, "Cannot allocate bzip2 decoder");
    size_t pos = 0;
    int result = -1;
    for (;;) {
        if (!z.avail_in && pos < length) {
            size_t n = chunk_size(length - pos);
            z.next_in = (char *)(uintptr_t)(source + pos);
            z.avail_in = (unsigned)n;
            pos += n;
        }
        unsigned before = z.avail_in;
        z.next_out = (char *)d->buffer;
        z.avail_out = (unsigned)d->capacity;
        int rc = BZ2_bzDecompress(&z);
        size_t produced = d->capacity - z.avail_out, used = pos - z.avail_in;
        if (write_output(d, produced, used)) break;
        if (rc == BZ_STREAM_END) {
            /* Streams may be concatenated, then followed by zero padding. */
            if (length - used < 4 || memcmp(source + used, "BZh", 3)) {
                size_t i = used;
                while (i < length && !source[i]) ++i;
                if (i < length) failure(d, "Unexpected data after the last stream");
                else result = 0;
                break;
            }
            char *next = z.next_in;
            unsigned avail = z.avail_in;
            BZ2_bzDecompressEnd(&z);
            memset(&z, 0, sizeof(z));
            if (BZ2_bzDecompressInit(&z, 0, 0) != BZ_OK) return failure(d, "Cannot allocate bzip2 decoder");
            z.next_in = next;
            z.avail_in = avail;
            continue;
        }
        if (rc != BZ_OK) {
            failure(d, rc == BZ_MEM_ERROR ? "Cannot allocate bzip2 decoder" : "Invalid bzip2 stream");
            break;
        }
        if (!produced && before == z.avail_in && pos == length) { failure(d, "Truncated input"); break; }
    }
    BZ2_bzDecompressEnd(&z);
    return result;
}
#endif
static int decode_zstd(Decoder *d, const unsigned char *source, size_t length) {
    ZSTD_DStream *z = ZSTD_createDStream();
    if (!z) return failure(d, "Cannot allocate zstd decoder");
    size_t rc = ZSTD_initDStream(z), pos = 0;
    int result = -1;
    if (ZSTD_isError(rc)) { failure(d, ZSTD_getErrorName(rc)); goto done; }
    ZSTD_inBuffer in = {source, 0, 0};
    for (;;) {
        if (cancelled(d)) break;
        if (in.pos == in.size) {
            size_t n = chunk_size(length - pos);
            in = (ZSTD_inBuffer){source + pos, n, 0};
            pos += n;
        }
        size_t before = in.pos;
        ZSTD_outBuffer out = {d->buffer, d->capacity, 0};
        rc = ZSTD_decompressStream(z, &out, &in);
        if (ZSTD_isError(rc)) { failure(d, ZSTD_getErrorName(rc)); break; }
        if (write_output(d, out.pos, pos - (in.size - in.pos))) break;
        if (!rc && in.pos == in.size && pos == length) { result = 0; break; }
        if (!out.pos && before == in.pos) {
            failure(d, "Truncated or invalid zstd stream"); break;
        }
    }
done:
    ZSTD_freeDStream(z);
    return result;
}
static const char *xz_error(lzma_ret rc) {
    switch (rc) {
    case LZMA_MEM_ERROR: return "Cannot allocate xz decoder memory";
    case LZMA_MEMLIMIT_ERROR: return "xz decoder needs more than the 512 MiB memory limit";
    case LZMA_FORMAT_ERROR: return "Invalid xz format";
    case LZMA_OPTIONS_ERROR: return "Unsupported xz compression options";
    case LZMA_UNSUPPORTED_CHECK: return "Unsupported xz integrity check";
    case LZMA_DATA_ERROR: return "Corrupt xz stream or checksum";
    case LZMA_BUF_ERROR: return "Truncated xz stream";
    default: return "xz decoder error";
    }
}
static int decode_xz(Decoder *d, const unsigned char *source, size_t length, unsigned threads) {
    lzma_stream z = LZMA_STREAM_INIT;
    uint32_t flags = LZMA_CONCATENATED | LZMA_TELL_UNSUPPORTED_CHECK;
    lzma_ret rc;
#if LZMA_VERSION >= 50040002
    if (threads > 1) {
        lzma_mt options = {0};
        options.flags = flags;
        options.threads = threads;
        options.timeout = 100; /* Poll cancellation while worker threads decode. */
        options.memlimit_threading = 256 * MIB;
        options.memlimit_stop = 512 * MIB;
        rc = lzma_stream_decoder_mt(&z, &options);
    } else
#else
    (void)threads;
#endif
        rc = lzma_stream_decoder(&z, 512 * MIB, flags);
    int result = -1;
    if (rc != LZMA_OK) { failure(d, xz_error(rc)); goto done; }
    size_t pos = 0;
    for (;;) {
        if (cancelled(d)) break;
        if (!z.avail_in) {
            size_t n = chunk_size(length - pos);
            z.next_in = source + pos;
            z.avail_in = n;
            pos += n;
        }
        z.next_out = d->buffer;
        z.avail_out = d->capacity;
        rc = lzma_code(&z, pos == length ? LZMA_FINISH : LZMA_RUN);
        if (write_output(d, d->capacity - z.avail_out, pos - z.avail_in)) break;
        if (rc == LZMA_STREAM_END) { result = 0; break; }
        if (rc != LZMA_OK) { failure(d, xz_error(rc)); break; }
    }
done:
    lzma_end(&z);
    return result;
}
/* Copies a pipe or device to the output, in memory or spilled to a file,
   after the `prefix` already read from it. */
static int read_stream(Decoder *d, int fd, const unsigned char *prefix, size_t prefix_length) {
    size_t total = 0;
    while (prefix_length) {
        size_t n = prefix_length < d->capacity ? prefix_length : d->capacity;
        memcpy(d->buffer, prefix, n);
        prefix += n;
        prefix_length -= n;
        total += n;
        if (write_output(d, n, total)) return -1;
    }
    for (;;) {
        if (cancelled(d)) return -1;
        ssize_t n = read(fd, d->buffer, d->capacity);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return failure(d, strerror(errno));
        if (!n) return 0;
        total += (size_t)n;
        if (write_output(d, (size_t)n, total)) return -1;
    }
}
static Compression detect(const unsigned char *p, size_t n) {
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) return COMP_GZIP;
    if (n >= 6 && !memcmp(p, "\xfd" "7zXZ\0", 6)) return COMP_XZ;
    /* "BZh", the block size, then a block or end-of-stream magic number. */
    if (n >= 10 && !memcmp(p, "BZh", 3) && p[3] >= '1' && p[3] <= '9' &&
        (!memcmp(p + 4, "\x31\x41\x59\x26\x53\x59", 6) || !memcmp(p + 4, "\x17\x72\x45\x38\x50\x90", 6)))
        return COMP_BZIP2;
    if (n >= 4 && ((p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f && p[3] == 0xfd) ||
                  ((p[0] & 0xf0) == 0x50 && p[1] == 0x2a && p[2] == 0x4d && p[3] == 0x18)))
        return COMP_ZSTD;
    return COMP_NONE;
}
static void stream_close(InputStream *s);
Compression input_detect(const unsigned char *data, size_t length) { return detect(data, length); }
void input_close(InputFile *file) {
    if (file->stream) {
        stream_close(file->stream);
        *file = (InputFile){.fd = -1};
        return;
    }
    if (file->size) munmap((void *)file->data, file->size);
    if (file->fd >= 0) close(file->fd);
    *file = (InputFile){.fd = -1};
}
/* Decodes `source` as `compression`, or with stream_fd >= 0 copies that
   stream, after `source` as what was read from it already, into memory or a
   temporary file. */
static int run_decoder(InputFile *file, const char *format, const unsigned char *source,
                       size_t length, int stream_fd, Compression compression,
                       const InputConfig *c, char *error, size_t error_size) {
    const char *temp_dir = c->temp_dir;
    size_t max_memory = c->max_memory;
    unsigned threads = c->threads;
    volatile sig_atomic_t *cancel = c->cancel;
    *file = (InputFile){.fd = -1};
    if (!temp_dir) temp_dir = getenv("TMPDIR");
    if (!temp_dir || !*temp_dir) temp_dir = "/tmp";
    Decoder d = {.fd = -1, .source_size = stream_fd >= 0 ? 0 : length, .cancel = cancel, .error = error,
                 .error_size = error_size, .format = format, .temp_dir = temp_dir,
                 .progress = !c->quiet && isatty(STDERR_FILENO), .stream = stream_fd >= 0};
    int result = -1;
    pthread_mutex_init(&d.lock, NULL);
    pthread_cond_init(&d.changed, NULL);
    /* Reserve address space only; pages become writable as output reaches
       them. Without the reservation, output goes straight to a file. */
    if (max_memory && max_memory <= SIZE_MAX - (size_t)sysconf(_SC_PAGESIZE) - COMMIT_STEP) {
        size_t reserved = page_round(max_memory), span = reserved + COMMIT_STEP;
        unsigned char *base = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            size_t head = round_up((uintptr_t)base, COMMIT_STEP) - (uintptr_t)base;
            if (head) munmap(base, head);
            munmap(base + head + reserved, span - head - reserved);
            d.memory = base + head;
            d.reserved = reserved;
            d.limit = max_memory;
            d.in_place = true;
#ifdef MADV_HUGEPAGE
            madvise(d.memory, reserved, MADV_HUGEPAGE);
#endif
        }
    }
    if (next_buffer(&d)) goto done;
    if (!threads) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        threads = cpus > 0 ? (unsigned)(cpus > 32 ? 32 : cpus) : 1;
    }
    if (length && stream_fd < 0) madvise((void *)source, length, MADV_SEQUENTIAL);
    if (d.progress) {
        fprintf(stderr, "%s %s... (Ctrl-C cancels)", d.stream ? "Reading" : "Decompressing", format);
        fflush(stderr);
    }
    clock_gettime(CLOCK_MONOTONIC, &d.last_update);
    int rc = d.stream ? read_stream(&d, stream_fd, source, length) :
             compression == COMP_GZIP ? decode_gzip(&d, source, length) :
             compression == COMP_ZSTD ? decode_zstd(&d, source, length) :
             compression == COMP_BZIP2 ? decode_bzip2(&d, source, length, threads) :
             decode_xz(&d, source, length, threads);
    if (d.threaded) {
        pthread_mutex_lock(&d.lock);
        int error = writer_idle(&d);
        d.stop = true;
        pthread_cond_broadcast(&d.changed);
        pthread_mutex_unlock(&d.lock);
        pthread_join(d.writer, NULL);
        d.threaded = false;
        if (!rc && error) rc = write_failure(&d, error);
    }
    if (d.progress) fputs("\r\033[K", stderr);
    if (rc || cancelled(&d)) goto done;
    const unsigned char *mapped = (const unsigned char *)"";
    bool anonymous = false;
    if (d.fd >= 0) {
        mapped = mmap(NULL, d.written, PROT_READ, MAP_PRIVATE, d.fd, 0);
        if (mapped == MAP_FAILED) { failure(&d, "Cannot mmap decompressed temporary file"); goto done; }
    } else if (d.written) {
        /* Keep only the used pages, read-only like a file mapping. */
        size_t used = page_round(d.written);
        if (used < d.reserved) munmap(d.memory + used, d.reserved - used);
        mprotect(d.memory, used, PROT_READ);
        mapped = d.memory;
        d.memory = NULL;
        anonymous = true;
    }
    *file = (InputFile){.data = mapped, .size = d.written, .fd = -1, .anonymous = anonymous};
    result = 0;
done:
    if (d.fd >= 0) close(d.fd);
    if (d.memory) munmap(d.memory, d.reserved);
    if (!d.in_place) free(d.buffer);
    free(d.spare);
    return result;
}
static const char *format_name(Compression c) {
    return c == COMP_GZIP ? "gzip" : c == COMP_ZSTD ? "zstd" : c == COMP_BZIP2 ? "bzip2" : "xz";
}
/* Decodes an already loaded document in place of itself when compressed. */
static int decode_loaded(InputFile *file, const InputConfig *c, char *error, size_t error_size) {
    Compression compression = c->compression == COMP_AUTO ? detect(file->data, file->size) : c->compression;
    if (compression == COMP_NONE) return 0;
    /* What is loaded counts against --max-mem, so the output gets what is
       left. */
    InputConfig budget = *c;
    size_t held = file->anonymous ? file->size : 0;
    budget.max_memory = held < budget.max_memory ? budget.max_memory - held : 0;
    InputFile decoded;
    int rc = run_decoder(&decoded, format_name(compression), file->data, file->size, -1,
                         compression, &budget, error, error_size);
    input_close(file);
    if (!rc) *file = decoded;
    return rc;
}

/* ---- Live streams ---- */
/* Address space reserved for a live stream so that its data never moves:
   memory up to the limit, then a temporary file mapped over the same
   addresses. Halved until a reservation succeeds. */
#define STREAM_RESERVE ((size_t)1 << 44)
#define STREAM_RESERVE_MIN ((size_t)1 << 30)
/* Read ahead at least this much, even with a smaller memory limit. */
#define STREAM_AHEAD ((size_t)64 << 20)
/* While input flows, news reaches the wake descriptor at most this often. */
#define NOTIFY_NS 50000000L
enum { PHASE_READING, PHASE_DECODING, PHASE_DONE };
struct InputStream {
    pthread_t thread;
    int fd, temp_fd;
    int control[2]; /* wakes the reader for input_demand and input_close */
    volatile sig_atomic_t stop;
    InputConfig config;
    char name[256];
    /* The reader's own: `base` holds `written` bytes, in anonymous memory up
       to `limit` (the first `committed` writable), then in temp_fd. */
    unsigned char *base;
    size_t reserved, limit, ahead, committed, written;
    Compression compression; /* COMP_AUTO until the first bytes decide */
    unsigned char head[10];
    size_t head_len;
    struct timespec notified;
    bool unnotified;
    /* Shared. `decoded` and `error` are final once phase is PHASE_DONE. */
    atomic_size_t available, wanted;
    atomic_int phase;
    atomic_bool compressed;
    InputFile decoded;
    bool decoded_ready;
    char error[512];
};
static int wake[2] = {-1, -1};
int input_wake_fd(void) { return wake[0]; }
void input_wake_drain(void) {
    char buf[64];
    if (wake[0] >= 0) while (read(wake[0], buf, sizeof(buf)) > 0) {}
}
static bool quiet_pipe(int fds[2]) {
    if (pipe(fds)) return false;
    for (int i = 0; i < 2; ++i) {
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    return true;
}
static void poke(int fd) {
    char c = 0;
    /* A full pipe already holds news. */
    if (write(fd, &c, 1) < 0) {}
}
static long since_notified(const InputStream *s) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - s->notified.tv_sec) * 1000000000L + (now.tv_nsec - s->notified.tv_nsec);
}
static void notify(InputStream *s) {
    poke(wake[1]);
    clock_gettime(CLOCK_MONOTONIC, &s->notified);
    s->unnotified = false;
}
static void publish(InputStream *s) {
    atomic_store_explicit(&s->available, s->written, memory_order_release);
    if (since_notified(s) >= NOTIFY_NS) notify(s);
    else s->unnotified = true;
}
static void finish(InputStream *s, const char *message) {
    if (message) snprintf(s->error, sizeof(s->error), "%s: %s", s->name, message);
    atomic_store_explicit(&s->phase, PHASE_DONE, memory_order_release);
    notify(s);
}
/* As detect(), but COMP_AUTO while the first bytes could still begin a
   compressed format. */
static Compression sniff(const unsigned char *p, size_t n) {
    Compression c = detect(p, n);
    if (c != COMP_NONE) return c;
    if ((n < 2 && !memcmp(p, "\x1f\x8b", n)) || (n < 6 && !memcmp(p, "\xfd" "7zXZ", n)) ||
        (n < 4 && (!memcmp(p, "\x28\xb5\x2f\xfd", n) ||
                   ((p[0] & 0xf0) == 0x50 && !memcmp(p + 1, "\x2a\x4d\x18", n - 1)))))
        return COMP_AUTO;
    if (n < 10 && !memcmp(p, "BZh", n < 3 ? n : 3) && (n < 4 || (p[3] >= '1' && p[3] <= '9')) &&
        (n < 5 || !memcmp(p + 4, "\x31\x41\x59\x26\x53\x59", n - 4) ||
         !memcmp(p + 4, "\x17\x72\x45\x38\x50\x90", n - 4)))
        return COMP_AUTO;
    return COMP_NONE;
}
/* Compressed input is needed whole; plain input is read ahead to `ahead`
   bytes and beyond that as far as input_demand asks. */
static bool may_read(InputStream *s) {
    return s->compression != COMP_NONE || s->written < s->ahead ||
           s->written < atomic_load_explicit(&s->wanted, memory_order_relaxed);
}
/* Reads once input is ready, sending news while waiting: bytes read, 0 at
   the end, -1 on error, -2 when stopped. */
static ssize_t stream_read(InputStream *s, unsigned char *dst, size_t room) {
    for (;;) {
        if (s->stop) return -2;
        int timeout = -1;
        if (s->unnotified) {
            long left = NOTIFY_NS - since_notified(s);
            if (left <= 0) { notify(s); continue; }
            timeout = (int)(left / 1000000) + 1;
        }
        bool wanted = may_read(s);
        struct pollfd p[2] = {{.fd = s->control[0], .events = POLLIN}, {.fd = s->fd, .events = POLLIN}};
        int ready = poll(p, wanted ? 2 : 1, timeout);
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        if (!ready) continue;
        if (p[0].revents) {
            char buf[64];
            while (read(s->control[0], buf, sizeof(buf)) > 0) {}
            continue;
        }
        ssize_t n = read(s->fd, dst, room);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return n;
    }
}
/* Memory is full: copies it to a temporary file, maps that over the same
   addresses (the bytes there do not change), and appends to it from now on. */
static bool stream_spill(InputStream *s, char *message, size_t cap) {
    int fd = make_temporary(s->config.temp_dir, message, cap);
    if (fd < 0) return false;
    int error = write_fd(fd, s->base, s->written, &s->stop);
    if (error) {
        snprintf(message, cap, "Temporary-file write failed: %s", error > 0 ? strerror(error) : "zero-length write");
        close(fd);
        return false;
    }
    if (!s->stop && mmap(s->base, s->reserved, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        snprintf(message, cap, "Cannot map temporary file: %s", strerror(errno));
        close(fd);
        return false;
    }
    s->temp_fd = fd;
    return true;
}
static void *stream_main(void *arg) {
    InputStream *s = arg;
    unsigned char *heap = NULL;
    char message[448] = "";
    bool failed = false;
    for (;;) {
        unsigned char *dst;
        size_t room;
        if (s->temp_fd < 0 && s->written < s->limit) {
            room = s->limit - s->written < BUFFER_SIZE ? s->limit - s->written : BUFFER_SIZE;
            size_t end = round_up(s->written + room, COMMIT_STEP);
            if (end > s->reserved) end = s->reserved;
            if (end > s->committed) {
                if (mprotect(s->base + s->committed, end - s->committed, PROT_READ | PROT_WRITE)) {
                    s->limit = s->written; /* Out of committable memory: spill early. */
                    continue;
                }
                s->committed = end;
            }
            dst = s->base + s->written;
        } else {
            if (s->temp_fd < 0 && !stream_spill(s, message, sizeof(message))) { failed = true; break; }
            if (!heap && !(heap = malloc(BUFFER_SIZE))) {
                snprintf(message, sizeof(message), "Cannot allocate a read buffer");
                failed = true; break;
            }
            room = s->reserved - s->written < BUFFER_SIZE ? s->reserved - s->written : BUFFER_SIZE;
            if (!room) {
                snprintf(message, sizeof(message), "Input exceeds the address space reserved for it");
                failed = true; break;
            }
            dst = heap;
        }
        ssize_t n = stream_read(s, dst, room);
        if (n == -2) break;
        if (n < 0) { snprintf(message, sizeof(message), "%s", strerror(errno)); failed = true; break; }
        if (!n) break;
        if (s->compression == COMP_AUTO) {
            size_t take = sizeof(s->head) - s->head_len < (size_t)n ? sizeof(s->head) - s->head_len : (size_t)n;
            memcpy(s->head + s->head_len, dst, take);
            s->head_len += take;
            s->compression = sniff(s->head, s->head_len);
            if (s->compression != COMP_AUTO && s->compression != COMP_NONE) {
                atomic_store(&s->compressed, true);
                notify(s);
            }
        }
        if (s->temp_fd >= 0) {
            int error = write_fd(s->temp_fd, heap, (size_t)n, &s->stop);
            if (s->stop) break;
            if (error) {
                snprintf(message, sizeof(message), "Temporary-file write failed: %s",
                         error > 0 ? strerror(error) : "zero-length write");
                failed = true; break;
            }
        }
        s->written += (size_t)n;
        if (s->compression == COMP_NONE) publish(s);
    }
    free(heap);
    close(s->fd);
    s->fd = -1;
    if (s->stop) return NULL;
    if (s->compression == COMP_AUTO) s->compression = COMP_NONE;
    if (s->compression == COMP_NONE || failed) {
        if (s->compression == COMP_NONE) {
            atomic_store_explicit(&s->available, s->written, memory_order_release);
            /* Return the unused reservation. */
            size_t used = page_round(s->written);
            if (used < s->reserved) { munmap(s->base + used, s->reserved - used); s->reserved = used; }
        }
        finish(s, failed ? message : NULL);
        return NULL;
    }
    atomic_store_explicit(&s->phase, PHASE_DECODING, memory_order_release);
    notify(s);
    /* Compressed input still in memory counts against --max-mem, so the
       output gets what is left, as in decode_loaded. */
    InputConfig budget = s->config;
    size_t held = s->temp_fd < 0 ? s->written : 0;
    budget.max_memory = held < budget.max_memory ? budget.max_memory - held : 0;
    int rc = run_decoder(&s->decoded, format_name(s->compression), s->base, s->written, -1,
                         s->compression, &budget, message, sizeof(message));
    munmap(s->base, s->reserved);
    s->base = NULL;
    s->reserved = 0;
    /* Set before the stop check, so that stream_free releases the output. */
    s->decoded_ready = !rc;
    if (s->stop) return NULL;
    finish(s, rc ? message : NULL);
    return NULL;
}
static void stream_free(InputStream *s) {
    if (s->decoded_ready) input_close(&s->decoded);
    if (s->reserved) munmap(s->base, s->reserved);
    if (s->fd >= 0) close(s->fd);
    if (s->temp_fd >= 0) close(s->temp_fd);
    if (s->control[0] >= 0) { close(s->control[0]); close(s->control[1]); }
    free(s);
}
static void stream_close(InputStream *s) {
    s->stop = 1;
    poke(s->control[1]);
    pthread_join(s->thread, NULL);
    stream_free(s);
}
/* Starts a live stream: 0, or -1 when the caller should read it whole. */
static int stream_start(InputFile *file, int fd, const char *name, const InputConfig *c) {
    InputStream *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->fd = s->temp_fd = s->control[0] = s->control[1] = -1;
    /* Some systems cannot poll devices; those are read whole. */
    struct pollfd probe = {.fd = fd, .events = POLLIN};
    if (poll(&probe, 1, 0) < 0 || (probe.revents & POLLNVAL) ||
        (wake[0] < 0 && !quiet_pipe(wake)) || !quiet_pipe(s->control) ||
        (s->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3)) < 0) {
        stream_free(s);
        return -1;
    }
    size_t reserve = STREAM_RESERVE;
    unsigned char *base = MAP_FAILED;
    for (; reserve >= STREAM_RESERVE_MIN; reserve /= 2) {
        base = mmap(NULL, reserve + COMMIT_STEP, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (base != MAP_FAILED) break;
    }
    if (base == MAP_FAILED) { stream_free(s); return -1; }
    size_t head = round_up((uintptr_t)base, COMMIT_STEP) - (uintptr_t)base;
    if (head) munmap(base, head);
    munmap(base + head + reserve, COMMIT_STEP - head);
    s->base = base + head;
    s->reserved = reserve;
    s->limit = c->max_memory < reserve ? c->max_memory : reserve;
    s->ahead = s->limit > STREAM_AHEAD ? s->limit : STREAM_AHEAD;
#ifdef MADV_HUGEPAGE
    if (s->limit) madvise(s->base, page_round(s->limit), MADV_HUGEPAGE);
#endif
    s->config = *c;
    if (!s->config.temp_dir) s->config.temp_dir = getenv("TMPDIR");
    if (!s->config.temp_dir || !*s->config.temp_dir) s->config.temp_dir = "/tmp";
    s->config.cancel = &s->stop;
    s->config.quiet = true;
    s->compression = c->compression;
    if (s->compression != COMP_AUTO && s->compression != COMP_NONE) atomic_init(&s->compressed, true);
    snprintf(s->name, sizeof(s->name), "%s", name);
#ifdef F_SETPIPE_SZ
    /* A larger pipe takes fewer wakeups to drain. */
    struct stat st;
    if (!fstat(s->fd, &st) && S_ISFIFO(st.st_mode)) fcntl(s->fd, F_SETPIPE_SZ, 1 << 20);
#endif
    /* Signals stay with the main thread, whose waits they must interrupt. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int rc = pthread_create(&s->thread, NULL, stream_main, s);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc) { stream_free(s); return -1; }
    *file = (InputFile){.data = (const unsigned char *)"", .fd = -1, .reading = true, .stream = s};
    return 0;
}
int input_update(InputFile *file, char *error, size_t error_size) {
    InputStream *s = file->stream;
    if (!s || !file->reading) return 0;
    bool done = atomic_load_explicit(&s->phase, memory_order_acquire) == PHASE_DONE;
    const unsigned char *d;
    size_t n;
    if (done && s->decoded_ready) { d = s->decoded.data; n = s->decoded.size; }
    else {
        n = atomic_load_explicit(&s->available, memory_order_acquire);
        d = n ? s->base : (const unsigned char *)"";
    }
    bool changed = done || d != file->data || n != file->size;
    file->data = d;
    file->size = n;
    if (!done) return changed;
    file->reading = false;
    if (s->error[0]) { snprintf(error, error_size, "%s", s->error); return -1; }
    return 1;
}
void input_demand(InputFile *file, size_t bytes) {
    InputStream *s = file->stream;
    if (!s || !file->reading) return;
    size_t old = atomic_exchange(&s->wanted, bytes);
    if (bytes > old) poke(s->control[1]);
}
const char *input_activity(const InputFile *file) {
    InputStream *s = file->stream;
    if (!s || !file->reading) return NULL;
    if (atomic_load(&s->phase) == PHASE_DECODING) return "decompressing";
    return atomic_load(&s->compressed) ? "reading compressed input" : "reading";
}
int input_open_fd(InputFile *file, int fd, const char *name, const InputConfig *c, bool live,
                  char *error, size_t error_size) {
    if (live && !stream_start(file, fd, name, c)) return 0;
    if (run_decoder(file, name, NULL, 0, fd, COMP_NONE, c, error, error_size)) return -1;
    return decode_loaded(file, c, error, error_size);
}
int input_copy(int fd, int out, const char *name, const InputConfig *c,
               char *error, size_t error_size) {
#ifdef F_SETPIPE_SZ
    fcntl(fd, F_SETPIPE_SZ, 1 << 20);
#endif
    size_t capacity = BUFFER_SIZE, have = 0;
    unsigned char *buffer = malloc(capacity);
    if (!buffer) { snprintf(error, error_size, "Cannot allocate a read buffer"); return -1; }
    int result = -1;
    bool end = false, splicing = true;
    /* Enough of the start to tell whether it is compressed. */
    Compression compression = c->compression;
    while (compression == COMP_AUTO && !*c->cancel) {
        ssize_t n = read(fd, buffer + have, capacity - have);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { snprintf(error, error_size, "%s: %s", name, strerror(errno)); goto done; }
        end = !n;
        have += (size_t)n;
        compression = end ? detect(buffer, have) : sniff(buffer, have);
    }
    if (*c->cancel) {
        snprintf(error, error_size, "%s: Reading interrupted", name);
    } else if (compression != COMP_NONE) {
        InputFile file;
        if (run_decoder(&file, name, buffer, have, fd, COMP_NONE, c, error, error_size) ||
            decode_loaded(&file, c, error, error_size)) goto done;
        int e = write_fd(out, file.data, file.size, c->cancel);
        input_close(&file);
        if (*c->cancel) snprintf(error, error_size, "%s: Interrupted", name);
        else if (e) snprintf(error, error_size, "Write failed: %s", e > 0 ? strerror(e) : "zero-length write");
        else result = 0;
    } else for (;;) {
        int e = write_fd(out, buffer, have, c->cancel);
        if (!e && !*c->cancel && end) { result = 0; break; }
#ifdef SPLICE_F_MOVE
        /* Between pipes, or a pipe and a file, the kernel moves the data. */
        if (!e && !*c->cancel && splicing) {
            ssize_t n;
            while ((n = splice(fd, NULL, out, NULL, 1 << 20, SPLICE_F_MOVE | SPLICE_F_MORE)) > 0 ||
                   (n < 0 && errno == EINTR && !*c->cancel)) {}
            if (!n) { result = 0; break; }
            if (*c->cancel) { snprintf(error, error_size, "%s: Reading interrupted", name); break; }
            if (errno != EINVAL) { snprintf(error, error_size, "Copy failed: %s", strerror(errno)); break; }
            splicing = false; /* Neither end is a pipe: copy through the buffer. */
        }
#endif
        ssize_t n = -1;
        while (!e && !*c->cancel && (n = read(fd, buffer, capacity)) < 0 && errno == EINTR) {}
        if (*c->cancel) { snprintf(error, error_size, "%s: Reading interrupted", name); break; }
        if (e) { snprintf(error, error_size, "Write failed: %s", e > 0 ? strerror(e) : "zero-length write"); break; }
        if (n < 0) { snprintf(error, error_size, "%s: %s", name, strerror(errno)); break; }
        have = (size_t)n;
        end = !n;
    }
done:
    free(buffer);
    return result;
}
int input_open(InputFile *file, const char *path, const InputConfig *c,
               char *error, size_t error_size) {
    *file = (InputFile){.fd = -1};
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { snprintf(error, error_size, "%s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st)) {
        snprintf(error, error_size, "fstat: %s", strerror(errno)); close(fd); return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        snprintf(error, error_size, "%s is a directory", path); close(fd); return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        if (!S_ISFIFO(st.st_mode) && !c->force) {
            snprintf(error, error_size, "%s is not a regular file (use -f to see it)", path);
            close(fd); return -1;
        }
        /* Pipes and devices are read as they are viewed. Opening a FIFO
           without O_NONBLOCK waits for a writer, as less does. */
        if (S_ISFIFO(st.st_mode)) {
            close(fd);
            do fd = open(path, O_RDONLY);
            while (fd < 0 && errno == EINTR && !*c->cancel);
            if (fd < 0) { snprintf(error, error_size, "%s: %s", path, strerror(errno)); return -1; }
        } else {
            int flags = fcntl(fd, F_GETFL);
            if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        if (c->pass_through) {
            int rc = input_copy(fd, STDOUT_FILENO, path, c, error, error_size);
            close(fd);
            return rc;
        }
        int rc = input_open_fd(file, fd, path, c, true, error, error_size);
        close(fd);
        if (!rc) file->once = true;
        return rc;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        snprintf(error, error_size, "A regular file fitting the address space is required");
        close(fd); return -1;
    }
    size_t length = (size_t)st.st_size;
    const unsigned char *source = length ? mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0) : (const unsigned char *)"";
    if (length && source == MAP_FAILED) {
        snprintf(error, error_size, "mmap: %s", strerror(errno)); close(fd); return -1;
    }
    Compression compression = c->compression == COMP_AUTO ? detect(source, length) : c->compression;
    if (compression == COMP_NONE) {
        /* The descriptor stays open so that F can follow growth. */
        *file = (InputFile){.data = source, .size = length, .fd = fd, .mapped = true,
                            .dev = (unsigned long long)st.st_dev, .ino = (unsigned long long)st.st_ino};
        return 0;
    }
    close(fd);
    int rc = run_decoder(file, format_name(compression), source, length, -1, compression, c,
                         error, error_size);
    if (length) munmap((void *)source, length);
    return rc;
}
int input_refresh(InputFile *file, char *error, size_t error_size) {
    if (!file->mapped || file->fd < 0) return 0;
    struct stat st;
    if (fstat(file->fd, &st)) { snprintf(error, error_size, "fstat: %s", strerror(errno)); return -1; }
    if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) return 0;
    size_t length = (size_t)st.st_size;
    if (length == file->size) return 0;
    const unsigned char *mapped = length ? mmap(NULL, length, PROT_READ, MAP_PRIVATE, file->fd, 0)
                                         : (const unsigned char *)"";
    if (length && mapped == MAP_FAILED) { snprintf(error, error_size, "mmap: %s", strerror(errno)); return -1; }
    if (file->size) munmap((void *)file->data, file->size);
    file->data = mapped;
    file->size = length;
    return 1;
}
