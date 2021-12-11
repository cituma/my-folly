#pragma once

#include <chrono>

enum class FutexResult {
    VALUE_CHANGED, /* futex value didn't match expected */
    AWOKEN, /* wakeup by matching futex wake, or spurious wakeup */
    INTERRUPTED, /* wakeup by interrupting signal */
    TIMEDOUT, /* wakeup by expiring deadline */
};

int nativeFutexWake(const void* addr, int count, uint32_t wakeMask);

FutexResult nativeFutexWaitImpl(
    const void* addr,
    uint32_t expected,
    std::chrono::system_clock::time_point const* absSystemTime,
    std::chrono::steady_clock::time_point const* absSteadyTime,
    uint32_t waitMask);
