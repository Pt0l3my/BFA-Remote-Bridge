// Only in the ThreadSanitizer build (BFARB_TSAN, with GCC): GCC 13's ThreadSanitizer doesn't intercept
// pthread_mutex_clocklock, which std::timed_mutex's try_lock_for and try_lock_until use, so it never sees those locks
// taken and reports their unlocks as unlocks of an unlocked mutex. This definition, in the test program, takes the
// place of the C library's and waits with pthread_mutex_timedlock instead, which ThreadSanitizer does intercept: the
// same deadline, moved to CLOCK_REALTIME. Never part of the core or the plugin.

#include <pthread.h>
#include <time.h>

extern "C" int pthread_mutex_clocklock(pthread_mutex_t* mutex, clockid_t clock, const struct timespec* deadline) {
    timespec onClock{}, onRealtime{};
    clock_gettime(clock, &onClock);
    clock_gettime(CLOCK_REALTIME, &onRealtime);
    long long left = (static_cast<long long>(deadline->tv_sec) - onClock.tv_sec) * 1000000000LL + (deadline->tv_nsec - onClock.tv_nsec);
    if (left < 0) left = 0;
    const long long at = static_cast<long long>(onRealtime.tv_sec) * 1000000000LL + onRealtime.tv_nsec + left;
    const timespec realtime{static_cast<time_t>(at / 1000000000LL), static_cast<long>(at % 1000000000LL)};
    return pthread_mutex_timedlock(mutex, &realtime);
}
