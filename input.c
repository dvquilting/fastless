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
#include <zlib.h>
#include <zstd.h>
#include <lzma.h>

#ifndef BUFFER_SIZE
#define BUFFER_SIZE (8u * 1024u * 1024u)
#endif
#define MIB UINT64_C(1048576)
typedef struct {
    int fd;
    size_t written, source_size;
    unsigned char *buffer;
    volatile sig_atomic_t *cancel;
    char *error;
    size_t error_size;
    const char *format;
    bool progress;
    struct timespec last_update;
} Decoder;

static int failure(Decoder *d, const char *message) {
    snprintf(d->error, d->error_size, "%s: %s", d->format, message);
    return -1;
}
static bool cancelled(Decoder *d) {
    if (!*d->cancel) return false;
    failure(d, "Decompression interrupted");
    return true;
}
static int write_output(Decoder *d, size_t length, size_t consumed) {
    if (length > SIZE_MAX - d->written ||
        (uintmax_t)d->written + length > INT64_MAX)
        return failure(d, "Decompressed file exceeds address/file size limits");
    size_t pos = 0;
    while (pos < length) {
        if (cancelled(d)) return -1;
        ssize_t n = write(d->fd, d->buffer + pos, length - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            char message[192];
            snprintf(message, sizeof(message), "Temporary-file write failed: %s", n ? strerror(errno) : "zero-length write");
            return failure(d, message);
        }
        pos += (size_t)n;
    }
    d->written += length;
    if (d->progress) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - d->last_update.tv_sec) +
                         (double)(now.tv_nsec - d->last_update.tv_nsec) / 1e9;
        if (elapsed >= 0.25) {
            fprintf(stderr, "\rDecompressing %s: %.1f%% input, %.2f GiB output\033[K",
                    d->format, d->source_size ? 100.0 * (double)consumed / (double)d->source_size : 100.0,
                    (double)d->written / (1024.0 * 1024.0 * 1024.0));
            fflush(stderr);
            d->last_update = now;
        }
    }
    return 0;
}
static size_t chunk_size(size_t remaining) {
    return remaining > BUFFER_SIZE ? BUFFER_SIZE : remaining;
}
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
        z.avail_out = BUFFER_SIZE;
        int rc = inflate(&z, Z_NO_FLUSH);
        size_t produced = BUFFER_SIZE - z.avail_out;
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
        ZSTD_outBuffer out = {d->buffer, BUFFER_SIZE, 0};
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
        z.avail_out = BUFFER_SIZE;
        rc = lzma_code(&z, pos == length ? LZMA_FINISH : LZMA_RUN);
        if (write_output(d, BUFFER_SIZE - z.avail_out, pos - z.avail_in)) break;
        if (rc == LZMA_STREAM_END) { result = 0; break; }
        if (rc != LZMA_OK) { failure(d, xz_error(rc)); break; }
    }
done:
    lzma_end(&z);
    return result;
}
static Compression detect(const unsigned char *p, size_t n) {
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) return COMP_GZIP;
    if (n >= 6 && !memcmp(p, "\xfd" "7zXZ\0", 6)) return COMP_XZ;
    if (n >= 4 && ((p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f && p[3] == 0xfd) ||
                  ((p[0] & 0xf0) == 0x50 && p[1] == 0x2a && p[2] == 0x4d && p[3] == 0x18)))
        return COMP_ZSTD;
    return COMP_NONE;
}
void input_close(InputFile *file) {
    if (file->size) munmap((void *)file->data, file->size);
    *file = (InputFile){0};
}
int input_open(InputFile *file, const char *path, Compression compression,
               const char *temp_dir, unsigned threads, volatile sig_atomic_t *cancel,
               char *error, size_t error_size) {
    *file = (InputFile){0};
    int source_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (source_fd < 0) { snprintf(error, error_size, "%s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(source_fd, &st)) {
        snprintf(error, error_size, "fstat: %s", strerror(errno)); close(source_fd); return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        snprintf(error, error_size, "A regular file fitting the address space is required");
        close(source_fd); return -1;
    }
    size_t length = (size_t)st.st_size;
    const unsigned char *source = length ? mmap(NULL, length, PROT_READ, MAP_PRIVATE, source_fd, 0) : (const unsigned char *)"";
    close(source_fd);
    if (length && source == MAP_FAILED) { snprintf(error, error_size, "mmap: %s", strerror(errno)); return -1; }
    if (compression == COMP_AUTO) compression = detect(source, length);
    if (compression == COMP_NONE) { *file = (InputFile){source, length}; return 0; }
    const char *format = compression == COMP_GZIP ? "gzip" : compression == COMP_ZSTD ? "zstd" : "xz";
    Decoder d = {.fd = -1, .source_size = length, .cancel = cancel, .error = error,
                 .error_size = error_size, .format = format, .progress = isatty(STDERR_FILENO)};
    int result = -1;
    if (!temp_dir) temp_dir = getenv("TMPDIR");
    if (!temp_dir || !*temp_dir) temp_dir = "/tmp";
    const char suffix[] = "/fastless-XXXXXX";
    size_t dirlen = strlen(temp_dir);
    char *name = dirlen <= SIZE_MAX - sizeof(suffix) ? malloc(dirlen + sizeof(suffix)) : NULL;
    if (!name) { failure(&d, "Cannot allocate temporary-file name"); goto done; }
    memcpy(name, temp_dir, dirlen);
    memcpy(name + dirlen, suffix, sizeof(suffix));
    d.fd = mkstemp(name);
    if (d.fd < 0) {
        snprintf(error, error_size, "Cannot create temporary file in %s: %s", temp_dir, strerror(errno));
        free(name); goto done;
    }
    /* No pathname survives cancellation, a crash, or successful mapping. */
    if (unlink(name)) {
        snprintf(error, error_size, "Cannot unlink temporary file: %s", strerror(errno));
        close(d.fd); d.fd = -1;
        unlink(name); free(name); goto done;
    }
    free(name);
    d.buffer = malloc(BUFFER_SIZE);
    if (!d.buffer) { failure(&d, "Cannot allocate decompression buffer"); goto done; }
    if (!threads) {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        threads = cpus > 0 ? (unsigned)(cpus > 32 ? 32 : cpus) : 1;
    }
    if (length) madvise((void *)source, length, MADV_SEQUENTIAL);
    if (d.progress) { fprintf(stderr, "Decompressing %s... (Ctrl-C cancels)", format); fflush(stderr); }
    clock_gettime(CLOCK_MONOTONIC, &d.last_update);
    int rc = compression == COMP_GZIP ? decode_gzip(&d, source, length) :
             compression == COMP_ZSTD ? decode_zstd(&d, source, length) : decode_xz(&d, source, length, threads);
    if (d.progress) fputs("\r\033[K", stderr);
    if (rc || cancelled(&d)) goto done;
    const unsigned char *mapped = d.written ? mmap(NULL, d.written, PROT_READ, MAP_PRIVATE, d.fd, 0) : (const unsigned char *)"";
    if (d.written && mapped == MAP_FAILED) { failure(&d, "Cannot mmap decompressed temporary file"); goto done; }
    *file = (InputFile){mapped, d.written};
    result = 0;
done:
    if (length) munmap((void *)source, length);
    if (d.fd >= 0) close(d.fd);
    free(d.buffer);
    return result;
}
