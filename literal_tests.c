#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "literal.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
static uint64_t state = UINT64_C(0x391abe17);
static unsigned next_random(void) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return (unsigned)state;
}
static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 32) : c;
}
static bool match(const unsigned char *data, const unsigned char *pattern, size_t len, bool fold) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char a = data[i], b = pattern[i];
        if (fold) { a = lower(a); b = lower(b); }
        if (a != b) return false;
    }
    return true;
}
static void verify(const unsigned char *data, size_t len, const unsigned char *text,
                   size_t text_len, bool fold, size_t before, bool avx2) {
    const unsigned char *first = NULL, *last = NULL;
    if (text_len <= len) {
        for (size_t i = 0; i <= len - text_len; ++i) {
            if (match(data + i, text, text_len, fold)) {
                if (!first) first = data + i;
                if (i < before) last = data + i;
            }
        }
    }
    Literal p;
    literal_init(&p, text, text_len, fold);
    volatile sig_atomic_t cancel = 0;
    const char *modes[] = {"off", "auto", "avx2"};
    for (size_t i = 0; i < (avx2 ? 3u : 2u); ++i) {
        if (!literal_simd_configure(modes[i])) abort();
        const unsigned char *a = literal_find(&p, data, len, &cancel);
        const unsigned char *b = literal_reverse(&p, data, len, before, &cancel);
        if (a != first || b != last) {
            fprintf(stderr, "%s mismatch: hay=%zu needle=%zu fold=%d before=%zu\n",
                    modes[i], len, text_len, fold, before);
            abort();
        }
        ++checks;
    }
}
int main(void) {
    bool avx2 = literal_simd_configure("avx2");
    unsigned char data[8192], text[1024];
    /* Randomized binary data, repeated alphabets, case changes, and overlaps. */
    for (unsigned trial = 0; trial < 6000; ++trial) {
        size_t len = next_random() % sizeof(data), text_len = 1 + next_random() % 513;
        bool fold = trial & 1;
        const char alphabet[] = "aAbB@[`{!?xX";
        for (size_t i = 0; i < len; ++i)
            data[i] = trial % 3 ? (unsigned char)alphabet[next_random() % (sizeof(alphabet) - 1)] : (unsigned char)next_random();
        for (size_t i = 0; i < text_len; ++i)
            text[i] = trial % 3 ? (unsigned char)alphabet[next_random() % (sizeof(alphabet) - 1)] : (unsigned char)next_random();
        if (text_len <= len && trial % 4) {
            size_t pos = next_random() % (len - text_len + 1);
            memcpy(data + pos, text, text_len);
            if (fold)
                for (size_t i = 0; i < text_len; ++i)
                    if (data[pos + i] >= 'a' && data[pos + i] <= 'z') data[pos + i] -= 32;
        }
        verify(data, len, text, text_len, fold, next_random() % (len + 2), avx2);
    }
    /* Both ends of a mapped page border inaccessible pages. A wide load or
       verification beyond the supplied range fails immediately. */
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *mapping = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || mprotect(mapping + page, page, PROT_READ | PROT_WRITE)) abort();
    size_t lengths[] = {1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511};
    for (size_t edge = 0; edge < 2; ++edge) {
        for (size_t len = 0; len <= 640; ++len) {
            unsigned char *hay = mapping + page + (edge ? page - len : 0);
            for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
                size_t text_len = lengths[n];
                memset(text, 'a', text_len);
                text[text_len / 2] = 'B';
                memset(hay, 'A', len);
                if (text_len <= len) memcpy(hay + len - text_len, text, text_len);
                for (size_t fold = 0; fold < 2; ++fold)
                    verify(hay, len, text, text_len, fold, len, avx2);
            }
        }
    }
    munmap(mapping, page * 3);
    /* Dense false candidates must keep ordering when the kernel falls back. */
    for (size_t text_len = 3; text_len <= 256; ++text_len) {
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = i & 1 ? 'b' : 'a';
        memcpy(text, data, text_len);
        text[text_len / 2] = 'Z';
        memcpy(data + 4001, text, text_len);
        for (size_t fold = 0; fold < 2; ++fold)
            verify(data, sizeof(data), text, text_len, fold, sizeof(data), avx2);
    }
    /* ASCII punctuation/high-bit bytes must not alias under case folding. */
    for (unsigned a = 0; a < 256; ++a) {
        memset(data, a, 128);
        for (unsigned b = 0; b < 256; ++b) {
            text[0] = (unsigned char)b;
            verify(data, 128, text, 1, true, 80, avx2);
        }
    }
    Literal p;
    literal_init(&p, "abc", 3, true);
    volatile sig_atomic_t cancel = 1;
    if (literal_find(&p, (const unsigned char *)"abc", 3, &cancel) ||
        literal_reverse(&p, (const unsigned char *)"abc", 3, 3, &cancel)) abort();
    puts(avx2 ? "AVX2 and portable kernels verified" : "Portable kernels verified (AVX2 unavailable)");
    printf("%lu literal differential checks passed\n", checks);
    return 0;
}
