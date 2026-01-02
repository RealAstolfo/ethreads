#ifndef ETHREADS_SHARED_STATE_CRTP_BASE_HPP
#define ETHREADS_SHARED_STATE_CRTP_BASE_HPP

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

#include "concepts.hpp"
#include "policies.hpp"

namespace ethreads {

// Forward declaration for scheduler integration
void schedule_coro_handle(std::coroutine_handle<> handle);

// =============================================================================
// Waiter Node for Async Primitives (Intrusive Linked List)
// =============================================================================

struct waiter_node {
  std::coroutine_handle<> handle{nullptr};
  waiter_node *next{nullptr};

  explicit waiter_node(std::coroutine_handle<> h) : handle(h), next(nullptr) {}
};

// =============================================================================
// Sync Primitive Base - Provides mutex + condition_variable pattern
// =============================================================================

template <typename Derived, typename LockPolicy = mutex_lock_policy>
class sync_primitive_base {
protected:
  using mutex_type = typename LockPolicy::mutex_type;
  using lock_type = typename LockPolicy::lock_type;

  mutable mutex_type mutex_;
  std::condition_variable_any cv_;

  // Wait for condition with timeout support
  template <typename Predicate>
  void wait_for_condition(lock_type &lock, Predicate pred) {
    cv_.wait(lock, pred);
  }

  template <typename Predicate, typename Rep, typename Period>
  bool wait_for_condition_for(lock_type &lock, Predicate pred,
                              std::chrono::duration<Rep, Period> timeout) {
    return cv_.wait_for(lock, timeout, pred);
  }

  template <typename Predicate, typename Clock, typename Duration>
  bool wait_for_condition_until(lock_type &lock, Predicate pred,
                                std::chrono::time_point<Clock, Duration> deadline) {
    return cv_.wait_until(lock, deadline, pred);
  }

  void notify_one() { cv_.notify_one(); }

  void notify_all() { cv_.notify_all(); }

  // CRTP access to derived class
  Derived &derived() { return static_cast<Derived &>(*this); }
  const Derived &derived() const { return static_cast<const Derived &>(*this); }

public:
  sync_primitive_base() = default;
  ~sync_primitive_base() = default;

  sync_primitive_base(const sync_primitive_base &) = delete;
  sync_primitive_base &operator=(const sync_primitive_base &) = delete;
  sync_primitive_base(sync_primitive_base &&) = delete;
  sync_primitive_base &operator=(sync_primitive_base &&) = delete;
};

// =============================================================================
// Async Primitive Base - Provides waiter list management
// =============================================================================

template <typename Derived> class async_primitive_base {
protected:
  std::atomic<waiter_node *> waiters_{nullptr};

  // Add a waiter to the front of the list (lock-free push)
  void add_waiter(waiter_node *node) {
    waiter_node *old_head = waiters_.load(std::memory_order_acquire);
    do {
      node->next = old_head;
    } while (!waiters_.compare_exchange_weak(old_head, node,
                                             std::memory_order_release,
                                             std::memory_order_acquire));
  }

  // Wake one waiter (pop from front)
  bool wake_one() {
    waiter_node *head = waiters_.load(std::memory_order_acquire);
    while (head != nullptr) {
      if (waiters_.compare_exchange_weak(head, head->next,
                                         std::memory_order_release,
                                         std::memory_order_acquire)) {
        if (head->handle) {
          schedule_coro_handle(head->handle);
        }
        return true;
      }
    }
    return false;
  }

  // Wake all waiters (swap with nullptr, then schedule all)
  void wake_all() {
    waiter_node *head =
        waiters_.exchange(nullptr, std::memory_order_acq_rel);
    while (head != nullptr) {
      waiter_node *next = head->next;
      if (head->handle) {
        schedule_coro_handle(head->handle);
      }
      head = next;
    }
  }

  // Check if there are waiters
  bool has_waiters() const {
    return waiters_.load(std::memory_order_acquire) != nullptr;
  }

  // CRTP access to derived class
  Derived &derived() { return static_cast<Derived &>(*this); }
  const Derived &derived() const { return static_cast<const Derived &>(*this); }

public:
  async_primitive_base() = default;
  ~async_primitive_base() = default;

  async_primitive_base(const async_primitive_base &) = delete;
  async_primitive_base &operator=(const async_primitive_base &) = delete;
  async_primitive_base(async_primitive_base &&) = delete;
  async_primitive_base &operator=(async_primitive_base &&) = delete;
};

// =============================================================================
// Awaitable Base - Provides standard coroutine awaiter interface via CRTP
// =============================================================================

template <typename Derived, typename T> class awaitable_base {
protected:
  // Derived class must implement:
  // - bool ready_impl() const
  // - void suspend_impl(std::coroutine_handle<> h)
  // - T resume_impl()

  Derived &derived() { return static_cast<Derived &>(*this); }
  const Derived &derived() const { return static_cast<const Derived &>(*this); }

public:
  bool await_ready() const noexcept { return derived().ready_impl(); }

  void await_suspend(std::coroutine_handle<> h) { derived().suspend_impl(h); }

  T await_resume() { return derived().resume_impl(); }
};

// Specialization for void
template <typename Derived> class awaitable_base<Derived, void> {
protected:
  Derived &derived() { return static_cast<Derived &>(*this); }
  const Derived &derived() const { return static_cast<const Derived &>(*this); }

public:
  bool await_ready() const noexcept { return derived().ready_impl(); }

  void await_suspend(std::coroutine_handle<> h) { derived().suspend_impl(h); }

  void await_resume() { derived().resume_impl(); }
};

// =============================================================================
// Version Tracking Mixin - For change detection
// =============================================================================

template <typename Derived> class version_tracking_mixin {
protected:
  std::atomic<std::uint64_t> version_{0};

  void increment_version() {
    version_.fetch_add(1, std::memory_order_release);
  }

public:
  std::uint64_t version() const {
    return version_.load(std::memory_order_acquire);
  }

  bool has_changed_since(std::uint64_t since_version) const {
    return version() != since_version;
  }
};

// =============================================================================
// Closeable Mixin - For channels and similar primitives
// =============================================================================

template <typename Derived> class closeable_mixin {
protected:
  std::atomic<bool> closed_{false};

public:
  void close() {
    closed_.store(true, std::memory_order_release);
    // Derived should override to wake waiters
    static_cast<Derived *>(this)->on_close();
  }

  bool is_closed() const {
    return closed_.load(std::memory_order_acquire);
  }

protected:
  // Default no-op, derived classes override
  void on_close() {}
};

// =============================================================================
// Result Holder - Type-erased result for async operations
// =============================================================================

template <typename T> class result_holder {
  std::optional<T> value_;
  std::exception_ptr exception_;

public:
  void set_value(T value) { value_ = std::move(value); }

  void set_exception(std::exception_ptr e) { exception_ = e; }

  bool has_value() const { return value_.has_value(); }

  bool has_exception() const { return exception_ != nullptr; }

  T get() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    return std::move(*value_);
  }

  const T &peek() const {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    return *value_;
  }
};

template <> class result_holder<void> {
  bool completed_{false};
  std::exception_ptr exception_;

public:
  void set_value() { completed_ = true; }

  void set_exception(std::exception_ptr e) { exception_ = e; }

  bool has_value() const { return completed_; }

  bool has_exception() const { return exception_ != nullptr; }

  void get() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }
};

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_CRTP_BASE_HPP
