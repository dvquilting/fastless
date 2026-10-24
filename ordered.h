/* What the threaded decoders (bzip2.c, gzip.c, zframes.c) share: how they
   hand output over in order, and how they wait and start their threads. */
#ifndef FASTLESS_ORDERED_H
#define FASTLESS_ORDERED_H
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>

/* Receives decoded bytes in order; `consumed` is how much of the input they
   come from. Returns nonzero to stop decoding. */
typedef int (*OrderedSink)(void *context, const unsigned char *data, size_t length, size_t consumed);
/* Memory for output bytes [offset, offset + length), or NULL. Other threads
   may copy those bytes there, and then pass the sink that memory. */
typedef unsigned char *(*OrderedPlace)(void *context, size_t offset, size_t length);

/* Waits on `changed` for at most 100 ms, so that the caller sees
   cancellation while workers are busy. */
static inline void ordered_wait(pthread_cond_t *changed, pthread_mutex_t *lock) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 100000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(changed, lock, &until);
}
/* Starts up to n threads running main(arg), with every signal blocked so
   that signals stay with the calling thread, which watches for
   cancellation. Returns how many started. */
static inline unsigned ordered_start(pthread_t *threads, unsigned n, void *(*main)(void *), void *arg) {
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    unsigned started = 0;
    while (started < n && !pthread_create(&threads[started], NULL, main, arg)) ++started;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return started;
}
#endif
