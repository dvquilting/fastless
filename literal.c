#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "literal.h"
#include <stdint.h>
#include <string.h>

#if !defined(FASTLESS_NO_AVX2) && (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#define HAVE_AVX2_KERNEL 1
#include <immintrin.h>
#define AVX2_TARGET __attribute__((target("avx2")))
#else
#define HAVE_AVX2_KERNEL 0
#endif
#define SCAN_CHUNK (8u * 1024u * 1024u)
static bool use_avx2, force_avx2;
static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}
bool literal_simd_configure(const char *mode) {
    bool available = false;
#if HAVE_AVX2_KERNEL
    __builtin_cpu_init();
    available = __builtin_cpu_supports("avx2") != 0;
#endif
    if (!strcmp(mode, "auto")) { use_avx2 = available; force_avx2 = false; }
    else if (!strcmp(mode, "off")) { use_avx2 = false; force_avx2 = false; }
    else if (!strcmp(mode, "avx2") && available) { use_avx2 = true; force_avx2 = true; }
    else return false;
    return true;
}
bool literal_avx2_enabled(void) { return use_avx2; }
void literal_init(Literal *p, const void *text, size_t length, bool fold) {
    *p = (Literal){.text = text, .len = length, .fold = fold};
    for (size_t i = 0; i < 256; ++i) p->skip[i] = p->reverse_skip[i] = length;
    if (!length) return;
    for (size_t i = 0; i + 1 < length; ++i)
        p->skip[fold ? lower(p->text[i]) : p->text[i]] = length - 1 - i;
    for (size_t i = length; i > 1; --i)
        p->reverse_skip[fold ? lower(p->text[i - 1]) : p->text[i - 1]] = i - 1;
    /* Prefer a different second byte to reject repetitive data cheaply. */
    p->anchor = length - 1;
    unsigned char first = fold ? lower(p->text[0]) : p->text[0];
    for (size_t i = length; i > 1; --i) {
        unsigned char c = fold ? lower(p->text[i - 1]) : p->text[i - 1];
        if (c != first) { p->anchor = i - 1; break; }
    }
}
static bool equal(const Literal *p, const unsigned char *data) {
    if (!p->fold) return !memcmp(data, p->text, p->len);
    for (size_t i = 0; i < p->len; ++i)
        if (lower(data[i]) != lower(p->text[i])) return false;
    return true;
}
static const unsigned char *find_portable(const Literal *p, const unsigned char *data,
                                          size_t length, volatile sig_atomic_t *cancel) {
    size_t last = length - p->len, pos = 0, ticks = 0;
    if (!p->fold) {
        while (pos <= last) {
            if (*cancel) return NULL;
            size_t n = last - pos + 1;
            if (n > SCAN_CHUNK) n = SCAN_CHUNK;
            const unsigned char *hit = memmem(data + pos, n + p->len - 1, p->text, p->len);
            if (hit) return hit;
            pos += n;
        }
    } else {
        while (pos <= last) {
            if (!(ticks++ & 65535) && *cancel) return NULL;
            size_t j = p->len;
            while (j && lower(data[pos + j - 1]) == lower(p->text[j - 1])) --j;
            if (!j) return data + pos;
            size_t step = p->skip[lower(data[pos + p->len - 1])];
            if (step > last - pos) break;
            pos += step;
        }
    }
    return NULL;
}
static const unsigned char *reverse_portable(const Literal *p, const unsigned char *data,
                                             size_t candidates, volatile sig_atomic_t *cancel) {
    size_t pos = candidates - 1, ticks = 0;
    for (;;) {
        if (!(ticks++ & 65535) && *cancel) return NULL;
        if (equal(p, data + pos)) return data + pos;
        unsigned char first = p->fold ? lower(data[pos]) : data[pos];
        size_t step = p->reverse_skip[first];
        if (step > pos) break;
        pos -= step;
    }
    return NULL;
}
#if HAVE_AVX2_KERNEL
/* For an ASCII letter, OR 0x20 maps exactly its upper/lowercase pair to
   lowercase. Nonletter anchors are compared without folding: punctuation and
   bytes >= 128 must never acquire false case-insensitive equivalences. */
AVX2_TARGET static __m256i anchor_matches(const unsigned char *data,
                                         __m256i needle, bool fold_letter) {
    __m256i bytes = _mm256_loadu_si256((const __m256i *)(const void *)data);
    if (fold_letter) bytes = _mm256_or_si256(bytes, _mm256_set1_epi8(0x20));
    return _mm256_cmpeq_epi8(bytes, needle);
}
AVX2_TARGET static __m256i candidate_matches(const Literal *p, const unsigned char *data,
                                            __m256i first, __m256i second,
                                            bool fold_a, bool fold_b) {
    __m256i matches = anchor_matches(data, first, fold_a);
    if (p->anchor)
        matches = _mm256_and_si256(matches, anchor_matches(data + p->anchor, second, fold_b));
    return matches;
}
static const unsigned char *verify_forward(const Literal *p, const unsigned char *data,
                                           unsigned mask, size_t *failed) {
    while (mask) {
        unsigned bit = (unsigned)__builtin_ctz(mask);
        if (equal(p, data + bit)) return data + bit;
        ++*failed;
        mask &= mask - 1;
    }
    return NULL;
}
AVX2_TARGET static const unsigned char *find_avx2(const Literal *p, const unsigned char *data,
                                                 size_t length, volatile sig_atomic_t *cancel) {
    size_t candidates = length - p->len + 1, pos = 0, ticks = 0;
    unsigned char a = p->fold ? lower(p->text[0]) : p->text[0];
    unsigned char b = p->fold ? lower(p->text[p->anchor]) : p->text[p->anchor];
    bool fold_a = p->fold && a >= 'a' && a <= 'z';
    bool fold_b = p->fold && b >= 'a' && b <= 'z';
    __m256i first = _mm256_set1_epi8((char)a), second = _mm256_set1_epi8((char)b);
    while (candidates - pos >= 128) {
        if (!(ticks++ & 4095) && *cancel) return NULL;
        __m256i m0 = candidate_matches(p, data + pos, first, second, fold_a, fold_b);
        __m256i m1 = candidate_matches(p, data + pos + 32, first, second, fold_a, fold_b);
        __m256i m2 = candidate_matches(p, data + pos + 64, first, second, fold_a, fold_b);
        __m256i m3 = candidate_matches(p, data + pos + 96, first, second, fold_a, fold_b);
        __m256i any = _mm256_or_si256(_mm256_or_si256(m0, m1), _mm256_or_si256(m2, m3));
        if (_mm256_movemask_epi8(any)) {
            const unsigned char *q;
            size_t failed = 0;
            q = verify_forward(p, data + pos, (unsigned)_mm256_movemask_epi8(m0), &failed);
            if (q) return q;
            q = verify_forward(p, data + pos + 32, (unsigned)_mm256_movemask_epi8(m1), &failed);
            if (q) return q;
            q = verify_forward(p, data + pos + 64, (unsigned)_mm256_movemask_epi8(m2), &failed);
            if (q) return q;
            q = verify_forward(p, data + pos + 96, (unsigned)_mm256_movemask_epi8(m3), &failed);
            if (q) return q;
            /* Dense false positives favor the skip-table/libc algorithm. */
            if (failed >= 16) {
                pos += 128;
                return pos < candidates ? find_portable(p, data + pos, length - pos, cancel) : NULL;
            }
        }
        pos += 128;
    }
    while (candidates - pos >= 32) {
        if (*cancel) return NULL;
        __m256i matches = candidate_matches(p, data + pos, first, second, fold_a, fold_b);
        unsigned mask = (unsigned)_mm256_movemask_epi8(matches);
        size_t failed = 0;
        const unsigned char *q = verify_forward(p, data + pos, mask, &failed);
        if (q) return q;
        pos += 32;
    }
    /* Never load a vector beyond the range, including mmap/page boundaries. */
    for (; pos < candidates; ++pos) if (equal(p, data + pos)) return data + pos;
    return NULL;
}
AVX2_TARGET static const unsigned char *reverse_avx2(const Literal *p, const unsigned char *data,
                                                    size_t candidates, volatile sig_atomic_t *cancel) {
    size_t ticks = 0;
    unsigned char a = p->fold ? lower(p->text[0]) : p->text[0];
    unsigned char b = p->fold ? lower(p->text[p->anchor]) : p->text[p->anchor];
    bool fold_a = p->fold && a >= 'a' && a <= 'z';
    bool fold_b = p->fold && b >= 'a' && b <= 'z';
    __m256i first = _mm256_set1_epi8((char)a), second = _mm256_set1_epi8((char)b);
    while (candidates >= 32) {
        if (!(ticks++ & 4095) && *cancel) return NULL;
        size_t pos = candidates - 32;
        __m256i matches = anchor_matches(data + pos, first, fold_a);
        if (p->anchor)
            matches = _mm256_and_si256(matches, anchor_matches(data + pos + p->anchor, second, fold_b));
        unsigned mask = (unsigned)_mm256_movemask_epi8(matches);
        size_t failed = 0;
        while (mask) {
            unsigned bit = 31u - (unsigned)__builtin_clz(mask);
            if (equal(p, data + pos + bit)) return data + pos + bit;
            ++failed;
            mask &= ~(1u << bit);
        }
        candidates = pos;
        if (failed >= 8)
            return candidates ? reverse_portable(p, data, candidates, cancel) : NULL;
    }
    while (candidates) {
        --candidates;
        if (equal(p, data + candidates)) return data + candidates;
    }
    return NULL;
}
#endif
const unsigned char *literal_find(const Literal *p, const unsigned char *data,
                                  size_t length, volatile sig_atomic_t *cancel) {
    if (!p->len || p->len > length || *cancel) return NULL;
#if HAVE_AVX2_KERNEL
    /* libc wins on short case-sensitive ranges on the tested platform. Keep
       its fast path there; folded scans and large ranges benefit from AVX2. */
    if (use_avx2 && (p->fold || length >= 4096 || force_avx2) &&
        p->len <= 256 && length - p->len >= 31)
        return find_avx2(p, data, length, cancel);
#endif
    return find_portable(p, data, length, cancel);
}
const unsigned char *literal_reverse(const Literal *p, const unsigned char *data,
                                     size_t length, size_t before,
                                     volatile sig_atomic_t *cancel) {
    if (!p->len || p->len > length || !before || *cancel) return NULL;
    size_t candidates = length - p->len + 1;
    if (candidates > before) candidates = before;
#if HAVE_AVX2_KERNEL
    if (use_avx2 && p->len <= 256 && candidates >= 32)
        return reverse_avx2(p, data, candidates, cancel);
#endif
    return reverse_portable(p, data, candidates, cancel);
}
