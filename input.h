#ifndef FASTLESS_INPUT_H
#define FASTLESS_INPUT_H
#include <stddef.h>
#include <signal.h>

typedef enum { COMP_AUTO, COMP_NONE, COMP_GZIP, COMP_ZSTD, COMP_XZ } Compression;
typedef struct {
    const unsigned char *data;
    size_t size;
} InputFile;

/* Returns 0 on success, -1 on error; compressed input is fully validated.
   Temporary files are unlinked immediately; the mapping owns their lifetime. */
int input_open(InputFile *file, const char *path, Compression compression,
               const char *temp_dir, unsigned threads,
               volatile sig_atomic_t *cancel, char *error, size_t error_size);
void input_close(InputFile *file);
#endif
