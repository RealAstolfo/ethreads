#ifndef ETHREADS_SHARED_STATE_EVENT_HPP
#define ETHREADS_SHARED_STATE_EVENT_HPP

#include <chrono>

#include "concepts.hpp"
#include "crtp_base.hpp"
#include "policies.hpp"
#include "timed_awaiter.hpp"

namespace ethreads {

// =============================================================================
// Sync Event - Boolean flag with reset policies
// =============================================================================

template <typename ResetPolicy = manual_reset_policy,
          typename LockPolicy = mutex_lock_policy>
class sync_event
    : public sync_primitive_base<sync_event<ResetPolicy, LockPolicy>, LockPolicy> {
  using base_type =
      sync_primitive_base<sync_event<ResetPolicy, LockPolicy>, LockPolicy>;

  bool signaled_{false};

public:
  using reset_policy = ResetPolicy;
  using lock_policy = LockPolicy;

  static constexpr bool is_auto_reset = ResetPolicy::auto_reset;

  explicit sync_event(bool initial_state = false) : signaled_(initial_state) {}

  // Set the event (signal)
  void set() {
    typename base_type::lock_type lock(this->mutex_);
    signaled_ = true;
    if constexpr (is_auto_reset) {
      this->notify_one(); // Auto-reset wakes one waiter
    } else {
      this->notify_all(); // Manual reset wakes all waiters
    }
    // lock released by RAII after notify
  }

  // Reset the event (unsignal)
  void reset() {
    typename base_type::lock_type lock(this->mutex_);
    signaled_ = false;
  }

  // Check if signaled without blocking
  bool is_set() const {
    typename base_type::lock_type lock(this->mutex_);
    return signaled_;
  }

  // Try to wait without blocking
  bool try_wait() {
    typename base_type::lock_type lock(this->mutex_);
    if (signaled_) {
      if constexpr (is_auto_reset) {
        signaled_ = false;
      }
      return true;
    }
    return false;
  }

  // Wait for event to be signaled (blocking)
  void wait() {
    typename base_type::lock_type lock(this->mutex_);
    this->wait_for_condition(lock, [this] { return signaled_; });
    if constexpr (is_auto_reset) {
      signaled_ = false;
    }
  }

  // Wait with timeout
  template <typename Rep, typename Period>
  bool wait_for(std::chrono::duration<Rep, Period> timeout) {
    typename base_type::lock_type lock(this->mutex_);
    if (this->wait_for_condition_for(lock, [this] { return signaled_; }, timeout)) {
      if constexpr (is_auto_reset) {
        signaled_ = false;
      }
      return true;
    }
    return false;
  }

  // Wait until deadline
  template <typename Clock, typename Duration>
  bool wait_until(std::chrono::time_point<Clock, Duration> deadline) {
    typename base_type::lock_type lock(this->mutex_);
    if (this->wait_for_condition_until(lock, [this] { return signaled_; },
                                       deadline)) {
      if constexpr (is_auto_reset) {
        signaled_ = false;
      }
      return true;
    }
    return false;
  }
};

// =============================================================================
// Async Event Awaiter
// =============================================================================

template <typename ResetPolicy, typename LockPolicy>
class async_event;

template <typename ResetPolicy, typename LockPolicy>
class event_wait_awaiter
    : public awaitable_base<event_wait_awaiter<ResetPolicy, LockPolicy>, void> {
  async_event<ResetPolicy, LockPolicy> &event_;
  waiter_node node_;

public:
  explicit event_wait_awaiter(async_event<ResetPolicy, LockPolicy> &event)
      : event_(event), node_(nullptr) {}

  bool ready_impl() const noexcept {
    // Check if already signaled
    return event_.try_wait();
  }

  void suspend_impl(std::coroutine_handle<> h) {
    node_.handle = h;
    event_.add_wait_waiter(&node_);
    // Re-check after adding to waiter list
    if (event_.is_set()) {
      // Event was signaled, we'll be woken
    }
  }

  void resume_impl() {
    // For auto-reset, the event was already consumed when we were woken
    // For manual-reset, multiple waiters can proceed
  }
};

// =============================================================================
// Async Event - Extends sync with coroutine-awaitable wait
// =============================================================================

template <typename ResetPolicy = manual_reset_policy,
          typename LockPolicy = mutex_lock_policy>
class async_event : public sync_event<ResetPolicy, LockPolicy>,
                    public async_primitive_base<async_event<ResetPolicy, LockPolicy>> {
  using sync_base = sync_event<ResetPolicy, LockPolicy>;
  using async_base = async_primitive_base<async_event<ResetPolicy, LockPolicy>>;

  friend class event_wait_awaiter<ResetPolicy, LockPolicy>;

public:
  using reset_policy = ResetPolicy;
  using lock_policy = LockPolicy;

  explicit async_event(bool initial_state = false) : sync_base(initial_state) {}

  // Inherit sync API
  using sync_base::is_set;
  using sync_base::reset;
  using sync_base::try_wait;
  using sync_base::wait_for;
  using sync_base::wait_until;

  // Override set to wake async waiters
  void set() {
    sync_base::set();
    if constexpr (sync_base::is_auto_reset) {
      this->wake_one();
    } else {
      this->wake_all();
    }
  }

  // Async wait - returns awaiter
  auto wait_async() { return event_wait_awaiter<ResetPolicy, LockPolicy>(*this); }

  // Async wait with timeout — returns true if signaled, false on timeout
  template <typename Rep, typename Period>
  auto wait_async_for(std::chrono::duration<Rep, Period> timeout) {
    return timed_event_wait_awaiter<ResetPolicy, LockPolicy>(
        *this, std::chrono::steady_clock::now() + timeout);
  }

  // co_await support
  auto operator co_await() { return wait_async(); }

  // Add waiter for wait notifications
  void add_wait_waiter(waiter_node *node) { this->add_waiter(node); }
};

// =============================================================================
// Type Aliases
// =============================================================================

using event = sync_event<manual_reset_policy>;
using auto_reset_event = sync_event<auto_reset_policy>;
using manual_reset_event = sync_event<manual_reset_policy>;

using async_manual_event = async_event<manual_reset_policy>;
using async_auto_event = async_event<auto_reset_policy>;

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_EVENT_HPP
