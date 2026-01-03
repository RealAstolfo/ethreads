#ifndef ETHREADS_SHARED_STATE_SEMAPHORE_HPP
#define ETHREADS_SHARED_STATE_SEMAPHORE_HPP

#include <chrono>
#include <cstddef>
#include <limits>

#include "concepts.hpp"
#include "crtp_base.hpp"
#include "policies.hpp"

namespace ethreads {

// =============================================================================
// Sync Semaphore - Counting semaphore with blocking operations
// =============================================================================

template <std::ptrdiff_t LeastMaxValue = std::numeric_limits<std::ptrdiff_t>::max(),
          typename LockPolicy = mutex_lock_policy>
class sync_semaphore
    : public sync_primitive_base<sync_semaphore<LeastMaxValue, LockPolicy>, LockPolicy> {
  using base_type =
      sync_primitive_base<sync_semaphore<LeastMaxValue, LockPolicy>, LockPolicy>;

  std::ptrdiff_t count_;

public:
  using lock_policy = LockPolicy;

  static constexpr std::ptrdiff_t max() noexcept { return LeastMaxValue; }

  explicit sync_semaphore(std::ptrdiff_t initial = 0) : count_(initial) {}

  // Acquire one permit (blocking)
  void acquire() {
    typename base_type::lock_type lock(this->mutex_);
    this->wait_for_condition(lock, [this] { return count_ > 0; });
    --count_;
  }

  // Try to acquire without blocking
  bool try_acquire() noexcept {
    typename base_type::lock_type lock(this->mutex_);
    if (count_ > 0) {
      --count_;
      return true;
    }
    return false;
  }

  // Try to acquire with timeout
  template <typename Rep, typename Period>
  bool try_acquire_for(std::chrono::duration<Rep, Period> timeout) {
    typename base_type::lock_type lock(this->mutex_);
    if (this->wait_for_condition_for(lock, [this] { return count_ > 0; }, timeout)) {
      --count_;
      return true;
    }
    return false;
  }

  // Try to acquire until deadline
  template <typename Clock, typename Duration>
  bool try_acquire_until(std::chrono::time_point<Clock, Duration> deadline) {
    typename base_type::lock_type lock(this->mutex_);
    if (this->wait_for_condition_until(lock, [this] { return count_ > 0; },
                                       deadline)) {
      --count_;
      return true;
    }
    return false;
  }

  // Release n permits
  void release(std::ptrdiff_t n = 1) {
    typename base_type::lock_type lock(this->mutex_);
    count_ += n;
    // Wake up to n waiters
    for (std::ptrdiff_t i = 0; i < n; ++i) {
      this->notify_one();
    }
    // lock released by RAII after notify
  }

  // Get current available count
  std::ptrdiff_t available() const noexcept {
    typename base_type::lock_type lock(this->mutex_);
    return count_;
  }
};

// =============================================================================
// Async Semaphore Awaiter
// =============================================================================

template <std::ptrdiff_t LeastMaxValue, typename LockPolicy>
class async_semaphore;

template <std::ptrdiff_t LeastMaxValue, typename LockPolicy>
class semaphore_acquire_awaiter
    : public awaitable_base<semaphore_acquire_awaiter<LeastMaxValue, LockPolicy>, void> {
  async_semaphore<LeastMaxValue, LockPolicy> &sem_;
  waiter_node node_;

public:
  explicit semaphore_acquire_awaiter(async_semaphore<LeastMaxValue, LockPolicy> &sem)
      : sem_(sem), node_(nullptr) {}

  bool ready_impl() const noexcept {
    // Try to acquire immediately
    return sem_.try_acquire();
  }

  void suspend_impl(std::coroutine_handle<> h) {
    node_.handle = h;
    sem_.add_acquire_waiter(&node_);
    // Re-check after adding to waiter list
    if (sem_.try_acquire()) {
      // We got it, but we're already suspended
      // The resume will happen, and we'll just return
    }
  }

  void resume_impl() {
    // Acquire was successful either in ready_impl or by being woken
  }
};

// =============================================================================
// Async Semaphore - Extends sync with coroutine-awaitable acquire
// =============================================================================

template <std::ptrdiff_t LeastMaxValue = std::numeric_limits<std::ptrdiff_t>::max(),
          typename LockPolicy = mutex_lock_policy>
class async_semaphore
    : public sync_semaphore<LeastMaxValue, LockPolicy>,
      public async_primitive_base<async_semaphore<LeastMaxValue, LockPolicy>> {
  using sync_base = sync_semaphore<LeastMaxValue, LockPolicy>;
  using async_base = async_primitive_base<async_semaphore<LeastMaxValue, LockPolicy>>;

  friend class semaphore_acquire_awaiter<LeastMaxValue, LockPolicy>;

public:
  using lock_policy = LockPolicy;

  explicit async_semaphore(std::ptrdiff_t initial = 0) : sync_base(initial) {}

  // Inherit sync API
  using sync_base::available;
  using sync_base::max;
  using sync_base::try_acquire;
  using sync_base::try_acquire_for;
  using sync_base::try_acquire_until;

  // Override release to wake async waiters
  void release(std::ptrdiff_t n = 1) {
    sync_base::release(n);
    // Wake async waiters
    for (std::ptrdiff_t i = 0; i < n && this->has_waiters(); ++i) {
      this->wake_one();
    }
  }

  // Async acquire - returns awaiter
  auto acquire_async() {
    return semaphore_acquire_awaiter<LeastMaxValue, LockPolicy>(*this);
  }

  // co_await support
  auto operator co_await() { return acquire_async(); }

  // Add waiter for acquire notifications
  void add_acquire_waiter(waiter_node *node) { this->add_waiter(node); }
};

// =============================================================================
// Type Aliases
// =============================================================================

using semaphore = sync_semaphore<>;
using binary_semaphore = sync_semaphore<1>;
using counting_semaphore = sync_semaphore<>;

template <std::ptrdiff_t N = std::numeric_limits<std::ptrdiff_t>::max()>
using async_counting_semaphore = async_semaphore<N>;

using async_binary_semaphore = async_semaphore<1>;

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_SEMAPHORE_HPP
