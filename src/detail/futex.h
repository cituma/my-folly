#pragma once

#include <atomic>
#include <chrono>
#include <cassert>
#include <stdint.h>

namespace myfolly {
namespace detail {

using Futex = std::atomic<uint32_t>;

enum class FutexResult {
    VALUE_CHANGED, /* futex value didn't match expected */
    AWOKEN, /* wakeup by matching futex wake, or spurious wakeup */
    INTERRUPTED, /* wakeup by interrupting signal */
    TIMEDOUT, /* wakeup by expiring deadline */
};

int nativeFutexWake(const void* addr, int count, uint32_t wakeMask);

/** Optimal when TargetClock is the same type as Clock.
 *
 *  Otherwise, both Clock::now() and TargetClock::now() must be invoked. */
template <typename TargetClock, typename Clock, typename Duration>
typename TargetClock::time_point time_point_conv(
    std::chrono::time_point<Clock, Duration> const& time) {
  using std::chrono::duration_cast;
  using TimePoint = std::chrono::time_point<Clock, Duration>;
  using TargetDuration = typename TargetClock::duration;
  using TargetTimePoint = typename TargetClock::time_point;
  if (time == TimePoint::max()) {
    return TargetTimePoint::max();
  } else if (std::is_same<Clock, TargetClock>::value) {
    // in place of time_point_cast, which cannot compile without if-constexpr
    auto const delta = time.time_since_epoch();
    return TargetTimePoint(duration_cast<TargetDuration>(delta));
  } else {
    // different clocks with different epochs, so non-optimal case
    auto const delta = time - Clock::now();
    return TargetClock::now() + duration_cast<TargetDuration>(delta);
  }
}

/*
 * addr: ��֮�乲���ڴ��ַ, ���ڱ�������״̬
 * expected: ��ֵ��addr�ڵ�ֵ��ͬ, �����ͬ�����޷�������
 *           ����������ǰ��Ҫ��ȡaddr�ڵ�ֵ����expected��
 *           ���ֵ���õ�Ŀ����Ϊ�˷�ֹ��ʧwake�Ĳ�����
 *           �����һ���߳��ڻ���ǰ�����ֵ��������֮���޸������ֵ��
 *           ��һ���߳�����ֵ�ı�֮���ҵ���FUTEX_WAIT֮ǰ, ִ����FUTEX_WAKE����,
 *           ������õ��߳̾ͻ�۲쵽��ֵ�任�����޷����ѡ�
 */
FutexResult nativeFutexWait(
    const void* addr,
    uint32_t expected,
    std::chrono::system_clock::time_point const* absSystemTime,
    std::chrono::steady_clock::time_point const* absSteadyTime,
    uint32_t waitMask);

template <typename Futex, class Clock, class Duration>
FutexResult futexWaitUntil(
        const Futex* futex,
        uint32_t expected,
        std::chrono::time_point<Clock, Duration> const& deadline,
        uint32_t waitMask) {
    using Target = typename std::conditional<
      Clock::is_steady,
    std::chrono::steady_clock,
    std::chrono::system_clock>::type;
    auto const converted = time_point_conv<Target>(deadline);
    return converted == Target::time_point::max()
      ? nativeFutexWait(futex, expected, nullptr, nullptr, waitMask)
      : nativeFutexWait(futex, expected, converted, waitMask);
}

template <typename Futex>
FutexResult futexWait(
        const Futex* futex, uint32_t expected, uint32_t waitMask) {
    auto rv = nativeFutexWait(futex, expected, nullptr, nullptr, waitMask);
    assert(rv != FutexResult::TIMEDOUT);
    return rv;
}

template <typename Futex>
int futexWake(const Futex* futex, int count, uint32_t wakeMask) {
    return nativeFutexWake(futex, count, wakeMask);
}

};  // namespace detail
};  // namespace myfolly
