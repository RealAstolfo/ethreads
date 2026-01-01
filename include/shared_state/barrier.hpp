#ifndef ETHREADS_SHARED_STATE_BARRIER_HPP
#define ETHREADS_SHARED_STATE_BARRIER_HPP

#include <cstddef>
#include <functional>

#include "concepts.hpp"
#include "crtp_base.hpp"
#include "policies.hpp"

namespace ethreads {

// =============================================================================
// Default Completion Function (no-op)
// =============================================================================

struct default_barrier_completion {
  void operator()() noexcept {}
};

// =============================================================================
// Arrival Token - Represents arrival at a barrier phase
// =============================================================================

class arrival_token {
  std::size_t phase_;

public:
  explicit arrival_token(std::size_t phase) : phase_(phase) {}

  std::size_t phase() const noexcept { return phase_; }

  bool operator==(const arrival_token &other) const noexcept {
    return phase_ == other.phase_;
  }

  bool operator!=(const arrival_token &other) const noexcept {
    return phase_ != other.phase_;
  }
};

// =============================================================================
// Sync Barrier - Generation-based thread coordination
// =============================================================================

template <typename CompletionFunction = default_barrier_completion,
          typename LockPolicy = mutex_lock_policy>
class sync_barrier
    : public sync_primitive_base<sync_barrier<CompletionFunction, LockPolicy>,
                                 LockPolicy> {
  using base_type = sync_primitive_base<sync_barrier<CompletionFunction, LockPolicy>,
                                        LockPolicy>;

  std::ptrdiff_t expected_;
  std::ptrdiff_t count_;
  std::size_t phase_{0};
  CompletionFunction completion_;

  void complete_phase() {
    completion_();
    count_ = expected_;
    ++phase_;
  }

public:
  using lock_policy = LockPolicy;
  using completion_function = CompletionFunction;

  explicit sync_barrier(std::ptrdiff_t expected,
                        CompletionFunction completion = CompletionFunction{})
      : expected_(expected), count_(expected),
        completion_(std::move(completion)) {}

  // Arrive at the barrier (decrement count), returns arrival token
  [[nodiscard]] arrival_token arrive(std::ptrdiff_t n = 1) {
    typename base_type::lock_type lock(this->mutex_);
    std::size_t current_phase = phase_;
    count_ -= n;
    if (count_ == 0) {
      complete_phase();
      lock.unlock();
      this->notify_all();
    }
    return arrival_token{current_phase};
  }

  // Wait for phase to complete
  void wait(arrival_token token) {
    typename base_type::lock_type lock(this->mutex_);
    this->wait_for_condition(lock, [this, &token] { return phase_ != token.phase(); });
  }

  // Arrive and wait in one operation
  void arrive_and_wait() {
    arrival_token token = arrive();
    wait(token);
  }

  // Arrive and drop out of future phases
  void arrive_and_drop() {
    typename base_type::lock_type lock(this->mutex_);
    --expected_;
    --count_;
    if (count_ == 0) {
      complete_phase();
      lock.unlock();
      this->notify_all();
    }
  }

  // Get current phase
  std::size_t phase() const {
    typename base_type::lock_type lock(this->mutex_);
    return phase_;
  }
};

// =============================================================================
// Async Barrier Awaiter
// =============================================================================

template <typename CompletionFunction, typename LockPolicy>
class async_barrier;

template <typename CompletionFunction, typename LockPolicy>
class barrier_wait_awaiter
    : public awaitable_base<barrier_wait_awaiter<CompletionFunction, LockPolicy>,
                            void> {
  async_barrier<CompletionFunction, LockPolicy> &barrier_;
  arrival_token token_;
  waiter_node node_;

public:
  barrier_wait_awaiter(async_barrier<CompletionFunction, LockPolicy> &barrier,
                       arrival_token token)
      : barrier_(barrier), token_(token), node_(nullptr) {}

  bool ready_impl() const noexcept {
    // Check if phase has already advanced
    return barrier_.phase() != token_.phase();
  }

  void suspend_impl(std::coroutine_handle<> h) {
    node_.handle = h;
    barrier_.add_phase_waiter(&node_);
    // Re-check after adding to waiter list
    if (barrier_.phase() != token_.phase()) {
      // Phase already advanced, we'll be woken
    }
  }

  void resume_impl() {
    // Phase completed, barrier advanced
  }
};

// =============================================================================
// Async Barrier - Extends sync with coroutine-awaitable wait
// =============================================================================

template <typename CompletionFunction = default_barrier_completion,
          typename LockPolicy = mutex_lock_policy>
class async_barrier
    : public sync_barrier<CompletionFunction, LockPolicy>,
      public async_primitive_base<async_barrier<CompletionFunction, LockPolicy>> {
  using sync_base = sync_barrier<CompletionFunction, LockPolicy>;
  using async_base =
      async_primitive_base<async_barrier<CompletionFunction, LockPolicy>>;

  friend class barrier_wait_awaiter<CompletionFunction, LockPolicy>;

public:
  using lock_policy = LockPolicy;
  using completion_function = CompletionFunction;

  explicit async_barrier(std::ptrdiff_t expected,
                         CompletionFunction completion = CompletionFunction{})
      : sync_base(expected, std::move(completion)) {}

  // Inherit sync API
  using sync_base::arrive_and_drop;
  using sync_base::phase;

  // Override arrive to wake async waiters when phase completes
  [[nodiscard]] arrival_token arrive(std::ptrdiff_t n = 1) {
    arrival_token token = sync_base::arrive(n);
    // If we completed the phase, wake all async waiters
    if (sync_base::phase() != token.phase()) {
      this->wake_all();
    }
    return token;
  }

  // Async wait - returns awaiter
  auto wait_async(arrival_token token) {
    return barrier_wait_awaiter<CompletionFunction, LockPolicy>(*this, token);
  }

  // Arrive and get awaiter for waiting
  auto arrive_and_wait_async() {
    arrival_token token = arrive();
    return wait_async(token);
  }

  // Add waiter for phase completion
  void add_phase_waiter(waiter_node *node) { this->add_waiter(node); }
};

// =============================================================================
// Type Aliases
// =============================================================================

using barrier = sync_barrier<>;

template <typename F>
using barrier_with_completion = sync_barrier<F>;

using async_sync_barrier = async_barrier<>;

template <typename F>
using async_barrier_with_completion = async_barrier<F>;

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_BARRIER_HPP
