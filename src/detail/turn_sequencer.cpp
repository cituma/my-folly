#include "detail/turn_sequencer.h"

namespace myfolly {
namespace detail {

void TurnSequencer::waitForTurn(const uint32_t turn,
        std::atomic<uint32_t>& spinCutoff,
        const bool updateSpinCutoff) {
    auto ret = tryWaitForTurn(turn, spinCutoff, updateSpinCutoff);
}

TryWaitResult TurnSequencer::tryWaitForTurn(const uint32_t turn,
        std::atomic<uint32_t>& spinCutoff,
        const bool updateSpinCutoff) {
    uint32_t prevThresh = spinCutoff.load(std::memory_order_relaxed);
    const uint32_t effectiveSpinCutoff =
        updateSpinCutoff || prevThresh == 0 ? kMaxSpinLimit : prevThresh;

    const uint32_t sturn = turn << kTurnShift;  //turn左移6位，以便与_state的前26bit比较
    for(uint32_t tries = 0;; ++tries) {
        uint32_t state = _state.load(std::memory_order_acquire);
        uint32_t current_sturn = decodeCurrentSturn(state); //将state后6bit设置为0
        if (current_sturn == sturn) {
            // 当前turn就是当前运行的轮次，直接返回，不需要等待。
            break;
        }

        // turn 比 current_sturn小，直接跳过
        if (sturn - current_sturn >= std::numeric_limits<uint32_t>::max() / 2) {
            // turn is in the past
            return TryWaitResult::PAST;
        }

        //--TODO

        // 当前最大的等待数量
        uint32_t current_max_waiter_delta = decodeMaxWaitersDelta(state);
        // 自己turn的等待数量
        uint32_t our_waiter_delta = (sturn - current_sturn) >> kTurnShift;

        uint32_t new_state;
        if(our_waiter_delta <= current_max_waiter_delta) {
            // 当前turn不是最新轮次，所以不需要更新state
            new_state = state;
        } else {
            // 当前turn是最新轮次，需要更新state
            new_state = encode(current_sturn, our_waiter_delta);
            if(state != new_state &&
                    !_state.compare_exchange_strong(state, new_state)) {
                // 在if判断期间其他线程有可能有更大的turn已经把_state更新了
                // 如果是这种情况, 那么直接继续下一轮次
                continue;
            }
            // 进入到该行说明_state更新成了new_state
        }
        // 等待new_state轮次的唤醒. 
        detail::futexWait(&_state, new_state, futexChannel(turn));
    }

    return TryWaitResult::SUCCESS;
}

// 临界区在waitForTurn(turn)与completeTurn(turn)之间.
// completeTurn(turn)将unblock一个阻塞在waitForTurncompleteTurn(turn + 1)的线程.
void TurnSequencer::completeTurn(const uint32_t turn) noexcept {
    uint32_t state = _state.load(std::memory_order_acquire);
    while(true) {
        uint32_t max_waiter_delta = decodeMaxWaitersDelta(state);
        // state的前26bit加1, 后bit减1
        uint32_t new_state = encode((turn + 1) << kTurnShift,
          max_waiter_delta == 0 ? 0 : max_waiter_delta - 1);
        if (_state.compare_exchange_strong(state, new_state)) {
            // _state 更新为 new_state
            if (max_waiter_delta != 0) {
                detail::futexWake(
                        &_state, std::numeric_limits<int>::max(), futexChannel(turn + 1));
            }
            break;
        }
        // _state值与state不同，说明由于其他线程已经更新过_state了。所以继续一次循环
    }
}

};  // namespace detail
};  // namespace myfolly
