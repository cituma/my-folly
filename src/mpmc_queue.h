#pragma once

#include "detail/turn_sequencer.h"
#include "portability.h"

namespace myfolly {
using namespace detail;

constexpr size_t hardware_destructive_interference_size =
    (kIsArchArm || kIsArchS390X) ? 64 : 128;

template <typename T>
class SingleElementQueue {
public:
    void enqueue(uint32_t turn,
            std::atomic<uint32_t>& spinCutoff,
            const bool updateSpinCutoff,
            const T& goner) {
        _sequencer.waitForTurn(turn * 2, spinCutoff, updateSpinCutoff);
        _contents = goner;
        _sequencer.completeTurn(turn * 2);
    }

    template <class Clock>
    bool tryWaitForEnqueueTurnUntil(
            const uint32_t turn,
            std::atomic<uint32_t>& spinCutoff,
            const bool updateSpinCutoff,
            const std::chrono::time_point<Clock>& when) noexcept {
        return _sequencer.tryWaitForTurn(
                turn * 2, spinCutoff, updateSpinCutoff, &when) !=
          TryWaitResult::TIMEDOUT;
    }

    bool mayEnqueue(const uint32_t turn) const noexcept {
        return _sequencer.isTurn(turn * 2);
    }

    void dequeue(uint32_t turn,
            std::atomic<uint32_t>& spinCutoff,
            const bool updateSpinCutoff,
            T& elem) {
        _sequencer.waitForTurn(turn * 2 + 1, spinCutoff, updateSpinCutoff);
        elem = _contents;
        _sequencer.completeTurn(turn * 2 + 1);
    }

    template <class Clock>
    bool tryWaitForDequeueTurnUntil(
            const uint32_t turn,
            std::atomic<uint32_t>& spinCutoff,
            const bool updateSpinCutoff,
            const std::chrono::time_point<Clock>& when) noexcept {
        return _sequencer.tryWaitForTurn(
                turn * 2 + 1, spinCutoff, updateSpinCutoff, &when) !=
          TryWaitResult::TIMEDOUT;
    }

    bool mayDequeue(const uint32_t turn) const noexcept {
        return _sequencer.isTurn(turn * 2 + 1);
    }

private:
    TurnSequencer _sequencer;
    T _contents;
};

template <typename T, typename Allocator = std::allocator<SingleElementQueue<T>>>
class MPMCQueue {
public:
    using Slot = SingleElementQueue<T>;
    explicit MPMCQueue(size_t const capacity,
            Allocator const& allocator = Allocator()) :
        _capacity(capacity),
        _allocator(allocator),
        _stride(computeStride(capacity)),
        _pushTicket(0),
        _popTicket(0),
        _pushSpinCutoff(0),
        _popSpinCutoff(0) {
        _slots = new Slot[capacity + 2 * this->kSlotPadding];
    }

    ~MPMCQueue() noexcept {
        delete[] _slots;
    }

    ssize_t size() const noexcept {
        uint64_t pushes = _pushTicket.load(std::memory_order_acquire); // A
        uint64_t pops = _popTicket.load(std::memory_order_acquire); // B
        while (true) {
            uint64_t nextPushes = _pushTicket.load(std::memory_order_acquire); // C
            if (pushes == nextPushes) {
                // _pushTicket didn't change from A (or the previous C) to C,
                // so we can linearize at B (or D)
                return ssize_t(pushes - pops);
            }
            pushes = nextPushes;
            uint64_t nextPops = _popTicket.load(std::memory_order_acquire); // D
            if (pops == nextPops) {
                // _popTicket didn't chance from B (or the previous D), so we
                // can linearize at C
                return ssize_t(pushes - pops);
            }
            pops = nextPops;
        }
    }

    bool isEmpty() const noexcept { return size() <= 0; }

    bool isFull() const noexcept {
        // careful with signed -> unsigned promotion, since size can be negative
        return size() >= static_cast<ssize_t>(_capacity);
    }

    size_t capacity() const noexcept { return _capacity; }

    bool write(T const& val) noexcept {
        uint64_t ticket;
        Slot* slots;
        size_t cap;
        int stride;
        if (tryObtainReadyPushTicket(ticket, slots, cap, stride)) {
            // we have pre-validated that the ticket won't block
            enqueueWithTicketBase(
                    ticket, slots, cap, stride, val);
            return true;
        } else {
            return false;
        }
    }

    void blockingWrite(T const& val) noexcept {
        enqueueWithTicketBase(
                _pushTicket++, _slots, _capacity, _stride, val);
    }

    template <class Clock>
    bool tryWriteUntil(
            const std::chrono::time_point<Clock>& when, T const& val) noexcept {
        uint64_t ticket;
        Slot* slots;
        size_t cap;
        int stride;
        if (tryObtainPromisedPushTicketUntil(ticket, slots, cap, stride, when)) {
            // we have pre-validated that the ticket won't block, or rather that
            // it won't block longer than it takes another thread to dequeue an
            // element from the slot it identifies.
            enqueueWithTicketBase(
                    ticket, slots, cap, stride, val);
            return true;
        } else {
            return false;
        }
    }

    bool read(T& elem) noexcept {
        uint64_t ticket = 0;
        Slot* slots = nullptr;
        size_t cap = 0;
        int stride = 0;
        if (tryObtainReadyPopTicket(ticket, slots, cap, stride)) {
            // the ticket has been pre-validated to not block
            dequeueWithTicketBase(ticket, slots, cap, stride, elem);
            return true;
        } else {
            return false;
        }
    }

    void blockingRead(T& elem) noexcept {
        uint64_t ticket = _popTicket++;
        dequeueWithTicketBase(ticket, _slots, _capacity, _stride, elem);
    }

    template <class Clock>
    bool tryReadUntil(
            const std::chrono::time_point<Clock>& when, T& elem) noexcept {
        uint64_t ticket;
        Slot* slots;
        size_t cap;
        int stride;
        if (tryObtainPromisedPopTicketUntil(ticket, slots, cap, stride, when)) {
            // we have pre-validated that the ticket won't block, or rather that
            // it won't block longer than it takes another thread to enqueue an
            // element on the slot it identifies.
            dequeueWithTicketBase(ticket, slots, cap, stride, elem);
            return true;
        } else {
            return false;
        }
    }

private:
    static int computeStride(size_t capacity) noexcept {
        static const int smallPrimes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23};

        int bestStride = 1;
        size_t bestSep = 1;
        for (int stride : smallPrimes) {
            if ((stride % capacity) == 0 || (capacity % stride) == 0) {
                continue;
            }
            size_t sep = stride % capacity;
            sep = std::min(sep, capacity - sep);
            if (sep > bestSep) {
                bestStride = stride;
                bestSep = sep;
            }
        }
        return bestStride;
    }

    /// Returns the index into _slots that should be used when enqueuing or
    /// dequeuing with the specified ticket
    size_t idx(uint64_t ticket, size_t cap, int stride) noexcept {
        return ((ticket * stride) % cap) + kSlotPadding;
    }

    /// Maps an enqueue or dequeue ticket to the turn should be used at the
    /// corresponding SingleElementQueue
    uint32_t turn(uint64_t ticket, size_t cap) noexcept {
        assert(cap != 0);
        return uint32_t(ticket / cap);
    }

    /// Tries to obtain a push ticket for which SingleElementQueue::enqueue
    /// won't block.  Returns true on immediate success, false on immediate
    /// failure.
    bool tryObtainReadyPushTicket(
            uint64_t& ticket, Slot*& slots, size_t& cap, int& stride) noexcept {
        ticket = _pushTicket.load(std::memory_order_acquire); // A
        slots = _slots;
        cap = _capacity;
        stride = _stride;
        while (true) {
            if (!slots[idx(ticket, cap, stride)].mayEnqueue(turn(ticket, cap))) {
                // if we call enqueue(ticket, ...) on the SingleElementQueue
                // right now it would block, but this might no longer be the next
                // ticket.  We can increase the chance of tryEnqueue success under
                // contention (without blocking) by rechecking the ticket dispenser
                auto prev = ticket;
                ticket = _pushTicket.load(std::memory_order_acquire); // B
                if (prev == ticket) {
                    // mayEnqueue was bracketed by two reads (A or prev B or prev
                    // failing CAS to B), so we are definitely unable to enqueue
                    return false;
                }
            } else {
                // we will bracket the mayEnqueue check with a read (A or prev B
                // or prev failing CAS) and the following CAS.  If the CAS fails
                // it will effect a load of _pushTicket
                if (_pushTicket.compare_exchange_strong(ticket, ticket + 1)) {
                    return true;
                }
            }
        }
    }

    /// Tries until when to obtain a push ticket for which
    /// SingleElementQueue::enqueue  won't block.  Returns true on success, false
    /// on failure.
    /// ticket is filled on success AND failure.
    template <class Clock>
      bool tryObtainPromisedPushTicketUntil(
              uint64_t& ticket,
              Slot*& slots,
              size_t& cap,
              int& stride,
              const std::chrono::time_point<Clock>& when) noexcept {
          bool deadlineReached = false;
          while (!deadlineReached) {
              if (tryObtainPromisedPushTicket(ticket, slots, cap, stride)) {
                  return true;
              }
              // ticket is a blocking ticket until the preceding ticket has been
              // processed: wait until this ticket's turn arrives. We have not reserved
              // this ticket so we will have to re-attempt to get a non-blocking ticket
              // if we wake up before we time-out.
              deadlineReached =
                !slots[idx(ticket, cap, stride)].tryWaitForEnqueueTurnUntil(
                        turn(ticket, cap),
                        _pushSpinCutoff,
                        (ticket % kAdaptationFreq) == 0,
                        when);
          }
          return false;
      }

    /// Tries to obtain a push ticket which can be satisfied if all
    /// in-progress pops complete.  This function does not block, but
    /// blocking may be required when using the returned ticket if some
    /// other thread's pop is still in progress (ticket has been granted but
    /// pop has not yet completed).
    bool tryObtainPromisedPushTicket(
            uint64_t& ticket, Slot*& slots, size_t& cap, int& stride) noexcept {
        auto numPushes = _pushTicket.load(std::memory_order_acquire); // A
        slots = _slots;
        cap = _capacity;
        stride = _stride;
        while (true) {
            ticket = numPushes;
            const auto numPops = _popTicket.load(std::memory_order_acquire); // B
            // n will be negative if pops are pending
            const int64_t n = int64_t(numPushes - numPops);
            if (n >= static_cast<ssize_t>(_capacity)) {
                // Full, linearize at B.  We don't need to recheck the read we
                // performed at A, because if numPushes was stale at B then the
                // real numPushes value is even worse
                return false;
            }
            if (_pushTicket.compare_exchange_strong(numPushes, numPushes + 1)) {
                return true;
            }
        }
    }

    /// Tries to obtain a pop ticket for which SingleElementQueue::dequeue
    /// won't block.  Returns true on immediate success, false on immediate
    /// failure.
    bool tryObtainReadyPopTicket(
            uint64_t& ticket, Slot*& slots, size_t& cap, int& stride) noexcept {
        ticket = _popTicket.load(std::memory_order_acquire);
        slots = _slots;
        cap = _capacity;
        stride = _stride;
        while (true) {
            if (!slots[idx(ticket, cap, stride)].mayDequeue(turn(ticket, cap))) {
                auto prev = ticket;
                ticket = _popTicket.load(std::memory_order_acquire);
                if (prev == ticket) {
                    return false;
                }
            } else {
                if (_popTicket.compare_exchange_strong(ticket, ticket + 1)) {
                    return true;
                }
            }
        }
    }

    /// Tries until when to obtain a pop ticket for which
    /// SingleElementQueue::dequeue won't block.  Returns true on success, false
    /// on failure.
    /// ticket is filled on success AND failure.
    template <class Clock>
      bool tryObtainPromisedPopTicketUntil(
              uint64_t& ticket,
              Slot*& slots,
              size_t& cap,
              int& stride,
              const std::chrono::time_point<Clock>& when) noexcept {
          bool deadlineReached = false;
          while (!deadlineReached) {
              if (tryObtainPromisedPopTicket(ticket, slots, cap, stride)) {
                  return true;
              }
              // ticket is a blocking ticket until the preceding ticket has been
              // processed: wait until this ticket's turn arrives. We have not reserved
              // this ticket so we will have to re-attempt to get a non-blocking ticket
              // if we wake up before we time-out.
              deadlineReached =
                !slots[idx(ticket, cap, stride)].tryWaitForDequeueTurnUntil(
                        turn(ticket, cap),
                        _pushSpinCutoff,
                        (ticket % kAdaptationFreq) == 0,
                        when);
          }
          return false;
      }

    /// Similar to tryObtainReadyPopTicket, but returns a pop ticket whose
    /// corresponding push ticket has already been handed out, rather than
    /// returning one whose corresponding push ticket has already been
    /// completed.  This means that there is a possibility that the caller
    /// will block when using the ticket, but it allows the user to rely on
    /// the fact that if enqueue has succeeded, tryObtainPromisedPopTicket
    /// will return true.  The "try" part of this is that we won't have
    /// to block waiting for someone to call enqueue, although we might
    /// have to block waiting for them to finish executing code inside the
    /// MPMCQueue itself.
    bool tryObtainPromisedPopTicket(
            uint64_t& ticket, Slot*& slots, size_t& cap, int& stride) noexcept {
        auto numPops = _popTicket.load(std::memory_order_acquire); // A
        slots = _slots;
        cap = _capacity;
        stride = _stride;
        while (true) {
            ticket = numPops;
            const auto numPushes = _pushTicket.load(std::memory_order_acquire); // B
            if (numPops >= numPushes) {
                // Empty, or empty with pending pops.  Linearize at B.  We don't
                // need to recheck the read we performed at A, because if numPops
                // is stale then the fresh value is larger and the >= is still true
                return false;
            }
            if (_popTicket.compare_exchange_strong(numPops, numPops + 1)) {
                return true;
            }
        }
    }

    // Given a ticket, constructs an enqueued item using args
    void enqueueWithTicketBase(
            uint64_t ticket,
            Slot* slots,
            size_t cap,
            int stride,
            const T& val) noexcept {
        slots[idx(ticket, cap, stride)].enqueue(
                turn(ticket, cap),
                _pushSpinCutoff,
                (ticket % kAdaptationFreq) == 0,
                val);
    }
    void dequeueWithTicketBase(
            uint64_t ticket, Slot* slots, size_t cap, int stride, T& elem) noexcept {
        assert(cap != 0);
        slots[idx(ticket, cap, stride)].dequeue(
                turn(ticket, cap),
                _popSpinCutoff,
                (ticket % kAdaptationFreq) == 0,
                elem);
    }

private:
    enum {
        /// Once every kAdaptationFreq we will spin longer, to try to estimate
        /// the proper spin backoff
        kAdaptationFreq = 128,

        /// To avoid false sharing in _slots with neighboring memory
        /// allocations, we pad it with this many SingleElementQueue-s at
        /// each end
        kSlotPadding =
          (hardware_destructive_interference_size - 1) / sizeof(Slot) + 1
    };

    alignas(hardware_destructive_interference_size) size_t _capacity;

    Allocator _allocator;
    Slot* _slots;

    int _stride;

    /// Enqueuers get tickets from here
    alignas(hardware_destructive_interference_size) std::atomic<uint64_t> _pushTicket;

    /// Dequeuers get tickets from here
    alignas(hardware_destructive_interference_size) std::atomic<uint64_t> _popTicket;

    /// This is how many times we will spin before using FUTEX_WAIT when
    /// the queue is full on enqueue, adaptively computed by occasionally
    /// spinning for longer and smoothing with an exponential moving average
    alignas(
            hardware_destructive_interference_size) std::atomic<uint32_t> _pushSpinCutoff;

    /// The adaptive spin cutoff when the queue is empty on dequeue
    alignas(hardware_destructive_interference_size) std::atomic<uint32_t> _popSpinCutoff;

    /// Alignment doesn't prevent false sharing at the end of the struct,
    /// so fill out the last cache line
    char _pad[hardware_destructive_interference_size - sizeof(std::atomic<uint32_t>)];
};

};  //namespace myfolly
