#include "detail/futex.h"

#ifdef __linux__
#include <linux/futex.h>      /* Definition of FUTEX_* constants */
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <unistd.h>
#endif
#include <errno.h>

namespace myfolly {
namespace detail {

using namespace std::chrono;

template <class Clock>
struct timespec timeSpecFromTimePoint(time_point<Clock> absTime) {
    auto epoch = absTime.time_since_epoch();
    if (epoch.count() < 0) {
        // kernel timespec_valid requires non-negative seconds and nanos in [0,1G)
        epoch = Clock::duration::zero();
    }

    // timespec-safe seconds and nanoseconds;
    // chrono::{nano,}seconds are `long long int`
    // whereas timespec uses smaller types
    using time_t_seconds = duration<std::time_t, seconds::period>;
    using long_nanos = duration<long int, nanoseconds::period>;

    auto secs = duration_cast<time_t_seconds>(epoch);
    auto nanos = duration_cast<long_nanos>(epoch - secs);
    struct timespec result = {secs.count(), nanos.count()};
    return result;
}

int nativeFutexWake(const void* addr, int count, uint32_t wakeMask) {
    int rv = syscall(
            __NR_futex,
            addr, /* addr1 */
            FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, /* op */
            count, /* val */
            nullptr, /* timeout */
            nullptr, /* addr2 */
            wakeMask); /* val3 */

    /* NOTE: we ignore errors on wake for the case of a futex
       guarding its own destruction, similar to this
       glibc bug with sem_post/sem_wait:
https://sourceware.org/bugzilla/show_bug.cgi?id=12674 */
    if (rv < 0) {
        return 0;
    }
    return rv;
}

FutexResult nativeFutexWait(
    const void* addr,
    uint32_t expected,
    system_clock::time_point const* absSystemTime,
    steady_clock::time_point const* absSteadyTime,
    uint32_t waitMask) {
    assert(absSystemTime == nullptr || absSteadyTime == nullptr);

    int op = FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG;
    struct timespec ts;
    struct timespec* timeout = nullptr;

    if (absSystemTime != nullptr) {
        op |= FUTEX_CLOCK_REALTIME;
        ts = timeSpecFromTimePoint(*absSystemTime);
        timeout = &ts;
    } else if (absSteadyTime != nullptr) {
        ts = timeSpecFromTimePoint(*absSteadyTime);
        timeout = &ts;
    }

    // Unlike FUTEX_WAIT, FUTEX_WAIT_BITSET requires an absolute timeout
    // value - http://locklessinc.com/articles/futex_cheat_sheet/
    int rv = syscall(
            __NR_futex,
            addr, /* addr1 */
            op, /* op */
            expected, /* val */
            timeout, /* timeout */
            nullptr, /* addr2 */
            waitMask); /* val3 */

    if (rv == 0) {
        return FutexResult::AWOKEN;
    } else {
        switch (errno) {
        case ETIMEDOUT:
          assert(timeout != nullptr);
          return FutexResult::TIMEDOUT;
        case EINTR:
          return FutexResult::INTERRUPTED;
        case EWOULDBLOCK:
          return FutexResult::VALUE_CHANGED;
        default:
          assert(false);
          // EINVAL, EACCESS, or EFAULT.  EINVAL means there was an invalid
          // op (should be impossible) or an invalid timeout (should have
          // been sanitized by timeSpecFromTimePoint).  EACCESS or EFAULT
          // means *addr points to invalid memory, which is unlikely because
          // the caller should have segfaulted already.  We can either
          // crash, or return a value that lets the process continue for
          // a bit. We choose the latter. VALUE_CHANGED probably turns the
          // caller into a spin lock.
          return FutexResult::VALUE_CHANGED;
        }
    }
}

};  // namespace detail
};  // namespace myfolly
