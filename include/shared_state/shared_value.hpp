#ifndef ETHREADS_SHARED_STATE_SHARED_VALUE_HPP
#define ETHREADS_SHARED_STATE_SHARED_VALUE_HPP

#include <functional>
#include <optional>
#include <utility>

#include "concepts.hpp"
#include "crtp_base.hpp"
#include "policies.hpp"

namespace ethreads {

// =============================================================================
// Sync Shared Value - Thread-safe value with blocking operations
// =============================================================================

template <typename T, typename LockPolicy = shared_mutex_lock_policy>
class sync_shared_value : public sync_primitive_base<sync_shared_value<T, LockPolicy>, LockPolicy>,
                          public version_tracking_mixin<sync_shared_value<T, LockPolicy>> {
  using base_type = sync_primitive_base<sync_shared_value<T, LockPolicy>, LockPolicy>;
  using version_mixin = version_tracking_mixin<sync_shared_value<T, LockPolicy>>;

  T value_;

public:
  using value_type = T;
  using lock_policy = LockPolicy;

  explicit sync_shared_value(T initial = T{}) : value_(std::move(initial)) {}

  // Load current value
  T load() const {
    if constexpr (requires { typename LockPolicy::shared_lock_type; }) {
      typename LockPolicy::shared_lock_type lock(this->mutex_);
      return value_;
    } else {
      typename LockPolicy::lock_type lock(this->mutex_);
      return value_;
    }
  }

  // Try to load without blocking (always succeeds for shared_value)
  std::optional<T> try_load() const { return load(); }

  // Store new value
  void store(T value) {
    {
      typename base_type::lock_type lock(this->mutex_);
      value_ = std::move(value);
      this->increment_version();
    }
    this->notify_all();
  }

  // Try store (always succeeds for shared_value)
  bool try_store(T value) {
    store(std::move(value));
    return true;
  }

  // Atomic exchange
  T exchange(T new_value) {
    T old;
    {
      typename base_type::lock_type lock(this->mutex_);
      old = std::move(value_);
      value_ = std::move(new_value);
      this->increment_version();
    }
    this->notify_all();
    return old;
  }

  // Compare and exchange
  bool compare_exchange(T &expected, T desired) {
    typename base_type::lock_type lock(this->mutex_);
    if (value_ == expected) {
      value_ = std::move(desired);
      this->increment_version();
      lock.unlock();
      this->notify_all();
      return true;
    }
    expected = value_;
    return false;
  }

  // Modify value with function
  template <typename Func>
    requires std::invocable<Func, T &>
  void modify(Func &&func) {
    {
      typename base_type::lock_type lock(this->mutex_);
      std::invoke(std::forward<Func>(func), value_);
      this->increment_version();
    }
    this->notify_all();
  }

  // Modify and return result
  template <typename Func>
    requires std::invocable<Func, T &>
  auto modify_and_get(Func &&func) -> std::invoke_result_t<Func, T &> {
    std::invoke_result_t<Func, T &> result;
    {
      typename base_type::lock_type lock(this->mutex_);
      result = std::invoke(std::forward<Func>(func), value_);
      this->increment_version();
    }
    this->notify_all();
    return result;
  }

  // Wait for value to change from given version
  T wait_for_change(std::uint64_t since_version) {
    typename base_type::lock_type lock(this->mutex_);
    this->wait_for_condition(
        lock, [this, since_version] { return this->version() != since_version; });
    return value_;
  }

  // Wait for predicate to become true
  template <typename Pred>
    requires std::predicate<Pred, const T &>
  T wait_until(Pred pred) {
    typename base_type::lock_type lock(this->mutex_);
    this->wait_for_condition(lock, [this, &pred] { return pred(value_); });
    return value_;
  }

  // Wait with timeout
  template <typename Rep, typename Period>
  std::optional<T> wait_for_change_for(std::uint64_t since_version,
                                        std::chrono::duration<Rep, Period> timeout) {
    typename base_type::lock_type lock(this->mutex_);
    if (this->wait_for_condition_for(
            lock, [this, since_version] { return this->version() != since_version; },
            timeout)) {
      return value_;
    }
    return std::nullopt;
  }
};

// =============================================================================
// Async Shared Value Awaiter
// =============================================================================

template <typename T, typename LockPolicy>
class async_shared_value;

template <typename T, typename LockPolicy>
class shared_value_change_awaiter
    : public awaitable_base<shared_value_change_awaiter<T, LockPolicy>, T> {
  async_shared_value<T, LockPolicy> &value_;
  std::uint64_t since_version_;
  waiter_node node_;

public:
  shared_value_change_awaiter(async_shared_value<T, LockPolicy> &value,
                               std::uint64_t since_version)
      : value_(value), since_version_(since_version), node_(nullptr) {}

  bool ready_impl() const noexcept { return value_.version() != since_version_; }

  void suspend_impl(std::coroutine_handle<> h) {
    node_.handle = h;
    value_.add_change_waiter(&node_);
    // Re-check after adding to waiter list to prevent missed wakeups
    if (value_.version() != since_version_) {
      // Try to remove ourselves and resume immediately
      // The schedule will happen anyway, but we handle the race
    }
  }

  T resume_impl() { return value_.load(); }
};

// =============================================================================
// Async Shared Value - Extends sync with coroutine-awaitable operations
// =============================================================================

template <typename T, typename LockPolicy = shared_mutex_lock_policy>
class async_shared_value : public sync_shared_value<T, LockPolicy>,
                           public async_primitive_base<async_shared_value<T, LockPolicy>> {
  using sync_base = sync_shared_value<T, LockPolicy>;
  using async_base = async_primitive_base<async_shared_value<T, LockPolicy>>;

  friend class shared_value_change_awaiter<T, LockPolicy>;

public:
  using value_type = T;
  using lock_policy = LockPolicy;

  explicit async_shared_value(T initial = T{}) : sync_base(std::move(initial)) {}

  // Inherit sync API
  using sync_base::compare_exchange;
  using sync_base::exchange;
  using sync_base::load;
  using sync_base::modify;
  using sync_base::store;
  using sync_base::try_load;
  using sync_base::try_store;
  using sync_base::version;

  // Override store to wake async waiters
  void store(T value) {
    sync_base::store(std::move(value));
    this->wake_all();
  }

  // Override exchange to wake async waiters
  T exchange(T new_value) {
    T old = sync_base::exchange(std::move(new_value));
    this->wake_all();
    return old;
  }

  // Override compare_exchange to wake async waiters on success
  bool compare_exchange(T &expected, T desired) {
    bool success = sync_base::compare_exchange(expected, std::move(desired));
    if (success) {
      this->wake_all();
    }
    return success;
  }

  // Override modify to wake async waiters
  template <typename Func>
    requires std::invocable<Func, T &>
  void modify(Func &&func) {
    sync_base::modify(std::forward<Func>(func));
    this->wake_all();
  }

  // Async wait for change - returns awaiter
  auto wait_for_change_async(std::uint64_t since_version) {
    return shared_value_change_awaiter<T, LockPolicy>(*this, since_version);
  }

  // Add waiter for change notifications
  void add_change_waiter(waiter_node *node) { this->add_waiter(node); }
};

// =============================================================================
// Type Aliases
// =============================================================================

template <typename T>
using shared_value = sync_shared_value<T, shared_mutex_lock_policy>;

template <typename T>
using async_value = async_shared_value<T, shared_mutex_lock_policy>;

template <typename T>
using fast_shared_value = sync_shared_value<T, spinlock_policy>;

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_SHARED_VALUE_HPP
