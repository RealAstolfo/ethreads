#ifndef ETHREADS_CONTINUATION_HANDOFF_HPP
#define ETHREADS_CONTINUATION_HANDOFF_HPP

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace ethreads {

// A lock-free, one-shot synchronization primitive for coroutine continuation handoff.
// Solves the race between a producer signaling completion and a consumer providing
// a continuation. Exactly one party wins and is responsible for resumption.
//
// State encoding (single atomic):
//   Bit 0: READY flag (producer has completed)
//   Bits 1+: continuation address (naturally aligned, so bit 0 is always 0)
//
// States:
//   0x0          = Initial (neither ready nor continuation set)
//   addr         = Continuation set, producer not ready yet
//   0x1          = Producer ready, no continuation yet
//   addr | 0x1   = Fully resolved (both set)
//
// Usage:
//   continuation_handoff handoff;
//
//   // Producer side (e.g., final_awaiter):
//   if (handoff.set_ready()) {
//     return handoff.get_continuation();  // Symmetric transfer
//   }
//
//   // Consumer side (e.g., await_suspend):
//   auto to_resume = handoff.set_continuation(awaiting);
//   if (to_resume != std::noop_coroutine()) {
//     schedule_coro_handle(to_resume);  // Producer already done
//   }
class continuation_handoff {
public:
  static constexpr uintptr_t READY_FLAG = 1;
  static constexpr uintptr_t ADDR_MASK = ~uintptr_t(1);

  continuation_handoff() = default;
  continuation_handoff(const continuation_handoff&) = delete;
  continuation_handoff& operator=(const continuation_handoff&) = delete;

  // Producer side: Signal that work is complete.
  // Returns true if THIS caller should resume the continuation.
  // Returns false if consumer will handle it (or no continuation yet).
  bool set_ready() noexcept {
    uintptr_t old_val = state_.load(std::memory_order_acquire);
    while (true) {
      if (old_val & READY_FLAG) {
        return false; // Already ready
      }
      uintptr_t new_val = old_val | READY_FLAG;
      if (state_.compare_exchange_weak(old_val, new_val,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
        return (old_val & ADDR_MASK) != 0; // True if continuation was set
      }
    }
  }

  // Consumer side: Provide a continuation to resume when ready.
  // Returns the continuation if producer already ready (caller should resume).
  // Returns noop_coroutine if producer will handle it later.
  std::coroutine_handle<> set_continuation(std::coroutine_handle<> h) noexcept {
    uintptr_t addr = reinterpret_cast<uintptr_t>(h.address());
    uintptr_t old_val = state_.load(std::memory_order_acquire);
    while (true) {
      if (old_val & ADDR_MASK) {
        return std::noop_coroutine(); // Already has continuation
      }
      uintptr_t new_val = (old_val & READY_FLAG) | addr;
      if (state_.compare_exchange_weak(old_val, new_val,
          std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (old_val & READY_FLAG) {
          return h; // Producer ready, we resume
        }
        return std::noop_coroutine(); // Producer will resume
      }
    }
  }

  // Get the stored continuation (if any)
  std::coroutine_handle<> get_continuation() const noexcept {
    uintptr_t val = state_.load(std::memory_order_acquire);
    void* addr = reinterpret_cast<void*>(val & ADDR_MASK);
    return addr ? std::coroutine_handle<>::from_address(addr)
                : std::noop_coroutine();
  }

  // Check if producer has signaled ready
  bool is_ready() const noexcept {
    return (state_.load(std::memory_order_acquire) & READY_FLAG) != 0;
  }

  // Check if continuation has been set
  bool has_continuation() const noexcept {
    return (state_.load(std::memory_order_acquire) & ADDR_MASK) != 0;
  }

  // Reset to initial state (for reuse - use with caution)
  void reset() noexcept {
    state_.store(0, std::memory_order_release);
  }

private:
  std::atomic<uintptr_t> state_{0};
};

} // namespace ethreads

#endif // ETHREADS_CONTINUATION_HANDOFF_HPP
