#ifndef FASTLESS_INPUT_H
#define FASTLESS_INPUT_H
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

typedef enum { COMP_AUTO, COMP_NONE, COMP_GZIP, COMP_ZSTD, COMP_XZ, COMP_BZIP2 } Compression;
typedef struct InputStream InputStream;
typedef struct {
    const unsigned char *data;
    size_t size;
    int fd;            /* kept open for a mapped regular file, else -1 */
    bool mapped;       /* data maps a regular file directly */
    bool reading;      /* a stream still being read: size grows on input_update */
    bool once;         /* from a pipe or device, so opening it again would not
                          give the same contents */
    bool anonymous;    /* data is anonymous memory, which counts against
                          max_memory, rather than a file mapping */
    unsigned long long dev, ino;
    InputStream *stream;
} InputFile;
typedef struct {
    Compression compression;
    const char *temp_dir;
    size_t max_memory;
    unsigned threads;
    bool force;        /* -f: read devices and other non-regular files */
    volatile sig_atomic_t *cancel;
    bool quiet;        /* no progress reports on standard error */
    bool pass_through; /* input_open copies pipes and devices to standard
                          output with input_copy, leaving the file empty */
} InputConfig;

/* Returns 0 on success, -1 on error; compressed input is fully validated.
   Regular files are mapped; compressed files, pipes, and devices are read
   into anonymous memory unless their contents exceed max_memory bytes, when
   they move to a temporary file in temp_dir. Temporary files are unlinked
   immediately; the mapping owns their lifetime. */
int input_open(InputFile *file, const char *path, const InputConfig *config,
               char *error, size_t error_size);
/* Reads a stream such as standard input. Without `live`, reads it to its end
   and then decodes it if it is compressed. With `live`, returns at once and
   reads it on a background thread: data stays at one address while size
   grows, as input_update publishes what has arrived. Compressed streams are
   read whole, then decoded, and appear when that finishes. Input that fits
   in max_memory is read ahead; beyond it, only as input_demand asks. The
   descriptor is not taken over; a live stream reads a duplicate. */
int input_open_fd(InputFile *file, int fd, const char *name, const InputConfig *config,
                  bool live, char *error, size_t error_size);
/* Copies a stream to `out` as it arrives, holding only a buffer, as cat
   does; compressed input is read whole and decoded first. Returns 0, or -1
   with a message (also when cancelled). */
int input_copy(int fd, int out, const char *name, const InputConfig *config,
               char *error, size_t error_size);
/* For a live stream: takes in what has arrived. Returns 1 if data, size, or
   reading changed, 0 if not, and -1 (with data and size updated, and reading
   false) if reading failed. */
int input_update(InputFile *file, char *error, size_t error_size);
/* Asks a live stream to read until it holds at least `bytes` (SIZE_MAX: to
   its end), past the read-ahead limit; 0 returns it to reading ahead only. */
void input_demand(InputFile *file, size_t bytes);
/* What a live stream is doing, for a status line, or NULL when it is done. */
const char *input_activity(const InputFile *file);
/* A descriptor that becomes readable when some live stream has news for
   input_update; input_wake_drain empties it. -1 before any live stream. */
int input_wake_fd(void);
void input_wake_drain(void);
/* The compressed format these first bytes begin, or COMP_NONE. Ten bytes
   are enough to tell. */
Compression input_detect(const unsigned char *data, size_t length);
/* Remaps a regular file whose size changed: 1 if it did, 0 if not, -1 on error. */
int input_refresh(InputFile *file, char *error, size_t error_size);
void input_close(InputFile *file);
#endif
