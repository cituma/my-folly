#pragma once

#include <atomic>
#include <algorithm>
#include <iostream>
#include <limits>
#include "detail/futex.h"
#include "portability.h"

namespace myfolly {
namespace detail {

enum class TryWaitResult { SUCCESS, PAST, TIMEDOUT };

class TurnSequencer {
private:
    static constexpr bool kSpinUsingHardwareClock = kIsArchAmd64;
    static constexpr uint32_t kCyclesPerSpinLimit =
      kSpinUsingHardwareClock ? 1 : 10;

    /// kTurnShift counts the bits that are stolen to record the delta
    /// between the current turn and the maximum waiter. It needs to be big
    /// enough to record wait deltas of 0 to 32 inclusive.  Waiters more
    /// than 32 in the future will be woken up 32*n turns early (since
    /// their BITSET will hit) and will adjust the waiter count again.
    /// We go a bit beyond and let the waiter count go up to 63, which is
    /// free and might save us a few CAS
    /// kTurnShift 值表示用于保存当前运行的turn和最大waiter差值的bits数量, 也就是说
    /// 有6 bits用于保存这个差值。这个值用于_state中
    static constexpr uint32_t kTurnShift = 6;
    static constexpr uint32_t kWaitersMask = (1 << kTurnShift) - 1;     //0x0000003F

    /// The minimum spin duration that we will adaptively select. The value
    /// here is cycles, adjusted to the way in which the limit will actually
    /// be applied.
    static constexpr uint32_t kMinSpinLimit = 200 / kCyclesPerSpinLimit;

    /// The maximum spin duration that we will adaptively select, and the
    /// spin duration that will be used when probing to get a new data
    /// point for the adaptation
    static constexpr uint32_t kMaxSpinLimit = 20000 / kCyclesPerSpinLimit;

public:
    explicit TurnSequencer(const uint32_t firstTurn = 0) :
        _state(encode(firstTurn << kTurnShift, 0)) {}
    ~TurnSequencer() {}

    bool isTurn(const uint32_t turn) const noexcept {
        auto state = _state.load(std::memory_order_acquire);
        return decodeCurrentSturn(state) == (turn << kTurnShift);
    }

    void waitForTurn(const uint32_t turn,
            std::atomic<uint32_t>& spinCutoff,
            const bool updateSpinCutoff);

    void completeTurn(const uint32_t turn) noexcept;

    TryWaitResult tryWaitForTurn(const uint32_t turn,
            std::atomic<uint32_t>& spinCutoff,
            const bool updateSpinCutoff);

private:
    uint32_t encode(uint32_t currentSturn, uint32_t maxWaiterD) const noexcept {
        // currentSturn后6bit都为0. 按位或maxWaiterD, 以获取完整的state值.
        return currentSturn | std::min(uint32_t{kWaitersMask}, maxWaiterD);
    }

    uint32_t decodeMaxWaitersDelta(uint32_t state) const noexcept {
        // 获取最大的等待数量
        return state & kWaitersMask;
    }

    uint32_t decodeCurrentSturn(uint32_t state) const noexcept {
        // 将state最6bit设置为0
        return state & ~kWaitersMask;
    }

    /// Returns the bitmask to pass futexWait or futexWake when communicating
    /// about the specified turn
    uint32_t futexChannel(uint32_t turn) const noexcept {
        return 1u << (turn & 31);
    }



private:
    /// 用_state保存当前运行状态，_state的前26bit保存当前运行的turn，
    /// 后6bit保存当前运行turn与最大等待turn的差值。
    Futex _state;
};

};  // namespace detail
};  // namespace myfolly
