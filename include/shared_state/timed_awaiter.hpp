#ifndef ETHREADS_SHARED_STATE_TIMED_AWAITER_HPP
#define ETHREADS_SHARED_STATE_TIMED_AWAITER_HPP

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <optional>

#include "crtp_base.hpp"
#include "../timer_service.hpp"

namespace ethreads {

// =============================================================================
// Async Operation Status
// =============================================================================

enum class async_op_status { completed, timed_out, closed };

// =============================================================================
// Timed Receive Result
// =============================================================================

template <typename T>
struct timed_receive_result {
  async_op_status status;
  std::optional<T> value;

  explicit operator bool() const { return status == async_op_status::completed; }
};

// =============================================================================
// Timed Awaiter Architecture
// =============================================================================
//
// Each timed awaiter races a timer against a primitive's waiter list.
// Both paths share a single atomic<bool> gate:
//
//   Timer path:  timer_service CAS(gate, false, true) -> schedule_coro_handle
//   Primitive:   wake_one/wake_all checks node.gate, CAS before scheduling
//
// Exactly one path wins the CAS and resumes the coroutine. In resume_impl,
// we try the operation (try_receive, try_acquire, etc.) to determine whether
// the primitive or timer fired.

// =============================================================================
// Timed Channel Receive Awaiter
// =============================================================================

template <typename T, typename BoundednessPolicy, typename LockPolicy>
class async_channel; // forward

template <typename T, typename BoundednessPolicy, typename LockPolicy>
class timed_channel_receive_awaiter
    : public awaitable_base<
          timed_channel_receive_awaiter<T, BoundednessPolicy, LockPolicy>,
          timed_receive_result<T>> {
  async_channel<T, BoundednessPolicy, LockPolicy> &channel_;
  std::chrono::steady_clock::time_point deadline_;
  std::atomic<bool> gate_{false};
  waiter_node node_;
  std::optional<T> result_;

public:
  timed_channel_receive_awaiter(
      async_channel<T, BoundednessPolicy, LockPolicy> &channel,
      std::chrono::steady_clock::time_point deadline)
      : channel_(channel), deadline_(deadline), node_(nullptr) {}

  bool ready_impl() noexcept {
    result_ = channel_.try_receive();
    if (result_.has_value())
      return true;
    if (channel_.is_closed())
      return true;
    if (std::chrono::steady_clock::now() >= deadline_)
      return true;
    return false;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    gate_.store(false, std::memory_order_relaxed);

    // Register timer — CAS(gate_, false, true) before scheduling h
    get_timer_service().add_timer(deadline_, h, nullptr, &gate_);

    // Register on channel waiter list with gated node
    node_.handle = h;
    node_.gate = &gate_;
    channel_.add_receive_waiter(&node_);

    // Re-check: data may have arrived between ready_impl and here
    result_ = channel_.try_receive();
    if (result_.has_value() || channel_.is_closed()) {
      bool expected = false;
      if (gate_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
        schedule_coro_handle(h);
      }
    }
  }

  timed_receive_result<T> resume_impl() {
    if (!result_.has_value())
      result_ = channel_.try_receive();

    if (result_.has_value())
      return {async_op_status::completed, std::move(result_)};
    if (channel_.is_closed())
      return {async_op_status::closed, std::nullopt};
    return {async_op_status::timed_out, std::nullopt};
  }
};

// =============================================================================
// Timed Channel Send Awaiter
// =============================================================================

template <typename T, typename BoundednessPolicy, typename LockPolicy>
class timed_channel_send_awaiter
    : public awaitable_base<
          timed_channel_send_awaiter<T, BoundednessPolicy, LockPolicy>,
          async_op_status> {
  async_channel<T, BoundednessPolicy, LockPolicy> &channel_;
  T value_;
  std::chrono::steady_clock::time_point deadline_;
  std::atomic<bool> gate_{false};
  waiter_node node_;
  bool sent_{false};

public:
  timed_channel_send_awaiter(
      async_channel<T, BoundednessPolicy, LockPolicy> &channel, T value,
      std::chrono::steady_clock::time_point deadline)
      : channel_(channel), value_(std::move(value)), deadline_(deadline),
        node_(nullptr) {}

  bool ready_impl() noexcept {
    if (channel_.is_closed())
      return true;
    sent_ = channel_.try_send(std::move(value_));
    if (sent_)
      return true;
    if (std::chrono::steady_clock::now() >= deadline_)
      return true;
    return false;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    gate_.store(false, std::memory_order_relaxed);

    get_timer_service().add_timer(deadline_, h, nullptr, &gate_);

    node_.handle = h;
    node_.gate = &gate_;
    channel_.add_send_waiter(&node_);

    sent_ = channel_.try_send(std::move(value_));
    if (sent_ || channel_.is_closed()) {
      bool expected = false;
      if (gate_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
        schedule_coro_handle(h);
      }
    }
  }

  async_op_status resume_impl() {
    if (sent_)
      return async_op_status::completed;
    if (channel_.is_closed())
      return async_op_status::closed;
    sent_ = channel_.try_send(std::move(value_));
    if (sent_)
      return async_op_status::completed;
    return async_op_status::timed_out;
  }
};

// =============================================================================
// Timed Event Wait Awaiter
// =============================================================================

template <typename ResetPolicy, typename LockPolicy>
class async_event; // forward

template <typename ResetPolicy, typename LockPolicy>
class timed_event_wait_awaiter
    : public awaitable_base<
          timed_event_wait_awaiter<ResetPolicy, LockPolicy>, bool> {
  async_event<ResetPolicy, LockPolicy> &event_;
  std::chrono::steady_clock::time_point deadline_;
  std::atomic<bool> gate_{false};
  waiter_node node_;

public:
  timed_event_wait_awaiter(async_event<ResetPolicy, LockPolicy> &event,
                           std::chrono::steady_clock::time_point deadline)
      : event_(event), deadline_(deadline), node_(nullptr) {}

  bool ready_impl() noexcept {
    if (event_.try_wait())
      return true;
    if (std::chrono::steady_clock::now() >= deadline_)
      return true;
    return false;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    gate_.store(false, std::memory_order_relaxed);

    get_timer_service().add_timer(deadline_, h, nullptr, &gate_);

    node_.handle = h;
    node_.gate = &gate_;
    event_.add_wait_waiter(&node_);

    if (event_.try_wait()) {
      bool expected = false;
      if (gate_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
        schedule_coro_handle(h);
      }
    }
  }

  // true = event signaled, false = timeout
  bool resume_impl() {
    return event_.try_wait() || event_.is_set();
  }
};

// =============================================================================
// Timed Semaphore Acquire Awaiter
// =============================================================================

template <std::ptrdiff_t LeastMaxValue, typename LockPolicy>
class async_semaphore; // forward

template <std::ptrdiff_t LeastMaxValue, typename LockPolicy>
class timed_semaphore_acquire_awaiter
    : public awaitable_base<
          timed_semaphore_acquire_awaiter<LeastMaxValue, LockPolicy>, bool> {
  async_semaphore<LeastMaxValue, LockPolicy> &sem_;
  std::chrono::steady_clock::time_point deadline_;
  std::atomic<bool> gate_{false};
  waiter_node node_;

public:
  timed_semaphore_acquire_awaiter(
      async_semaphore<LeastMaxValue, LockPolicy> &sem,
      std::chrono::steady_clock::time_point deadline)
      : sem_(sem), deadline_(deadline), node_(nullptr) {}

  bool ready_impl() noexcept {
    if (sem_.try_acquire())
      return true;
    if (std::chrono::steady_clock::now() >= deadline_)
      return true;
    return false;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    gate_.store(false, std::memory_order_relaxed);

    get_timer_service().add_timer(deadline_, h, nullptr, &gate_);

    node_.handle = h;
    node_.gate = &gate_;
    sem_.add_acquire_waiter(&node_);

    if (sem_.try_acquire()) {
      bool expected = false;
      if (gate_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
        schedule_coro_handle(h);
      }
    }
  }

  // true = acquired, false = timeout
  bool resume_impl() {
    return sem_.try_acquire();
  }
};

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_TIMED_AWAITER_HPP
