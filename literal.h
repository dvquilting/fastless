#ifndef FASTLESS_LITERAL_H
#define FASTLESS_LITERAL_H
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

typedef struct {
    const unsigned char *text;
    size_t len, anchor, skip[256], reverse_skip[256];
    bool fold;
} Literal;

/* No -mavx2 global compiler flag: unsupported CPUs keep the portable path. */
bool literal_simd_configure(const char *mode);
bool literal_avx2_enabled(void);
void literal_init(Literal *pattern, const void *text, size_t length, bool fold);
const unsigned char *literal_find(const Literal *pattern, const unsigned char *data,
                                  size_t length, volatile sig_atomic_t *cancel);
/* Match starts must be strictly less than before; matches may end after it. */
const unsigned char *literal_reverse(const Literal *pattern, const unsigned char *data,
                                     size_t length, size_t before,
                                     volatile sig_atomic_t *cancel);
#endif
