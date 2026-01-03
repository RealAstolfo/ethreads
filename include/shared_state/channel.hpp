#ifndef ETHREADS_SHARED_STATE_CHANNEL_HPP
#define ETHREADS_SHARED_STATE_CHANNEL_HPP

#include <chrono>
#include <deque>
#include <optional>
#include <utility>

#include "concepts.hpp"
#include "crtp_base.hpp"
#include "policies.hpp"

namespace ethreads {

// =============================================================================
// Channel Result - Result type for channel operations
// =============================================================================

enum class channel_op_status {
  success,
  closed,
  would_block,
  timeout
};

template <typename T>
struct channel_result {
  std::optional<T> value;
  channel_op_status status;

  explicit operator bool() const { return status == channel_op_status::success; }

  T &operator*() { return *value; }
  const T &operator*() const { return *value; }
};

// =============================================================================
// Sync Channel - Thread-safe message passing channel
// =============================================================================

template <typename T,
          typename BoundednessPolicy = unbounded_policy,
          typename LockPolicy = mutex_lock_policy>
class sync_channel
    : public sync_primitive_base<sync_channel<T, BoundednessPolicy, LockPolicy>,
                                 LockPolicy>,
      public closeable_mixin<sync_channel<T, BoundednessPolicy, LockPolicy>> {
  using base_type =
      sync_primitive_base<sync_channel<T, BoundednessPolicy, LockPolicy>,
                          LockPolicy>;
  using closeable = closeable_mixin<sync_channel<T, BoundednessPolicy, LockPolicy>>;

  friend closeable;

  std::deque<T> buffer_;
  std::condition_variable_any send_cv_;

  void on_close() {
    typename base_type::lock_type lock(this->mutex_);
    this->notify_all();
    send_cv_.notify_all();
    // lock released by RAII after notify
  }

  bool can_send() const {
    if constexpr (BoundednessPolicy::is_bounded) {
      return buffer_.size() < BoundednessPolicy::max_size();
    } else {
      return true;
    }
  }

public:
  using value_type = T;
  using boundedness_policy = BoundednessPolicy;
  using lock_policy = LockPolicy;

  static constexpr bool is_bounded = BoundednessPolicy::is_bounded;
  static constexpr std::size_t capacity() { return BoundednessPolicy::max_size(); }

  sync_channel() = default;

  // Send a value (blocking for bounded channels)
  void send(T value) {
    typename base_type::lock_type lock(this->mutex_);
    if constexpr (is_bounded) {
      send_cv_.wait(lock, [this] {
        return this->is_closed() || can_send();
      });
    }
    if (this->is_closed()) {
      throw std::runtime_error("send on closed channel");
    }
    buffer_.push_back(std::move(value));
    this->notify_one();
    // lock released by RAII after notify
  }

  // Try to send without blocking
  bool try_send(T value) {
    typename base_type::lock_type lock(this->mutex_);
    if (this->is_closed()) {
      return false;
    }
    if constexpr (is_bounded) {
      if (!can_send()) {
        return false;
      }
    }
    buffer_.push_back(std::move(value));
    this->notify_one();
    return true;
    // lock released by RAII after notify
  }

  // Send with timeout (only meaningful for bounded channels)
  template <typename Rep, typename Period>
  bool send_for(T value, std::chrono::duration<Rep, Period> timeout) {
    typename base_type::lock_type lock(this->mutex_);
    if constexpr (is_bounded) {
      if (!send_cv_.wait_for(lock, timeout, [this] {
            return this->is_closed() || can_send();
          })) {
        return false;
      }
    }
    if (this->is_closed()) {
      return false;
    }
    buffer_.push_back(std::move(value));
    this->notify_one();
    return true;
    // lock released by RAII after notify
  }

  // Receive a value (blocking)
  T receive() {
    typename base_type::lock_type lock(this->mutex_);
    this->wait_for_condition(lock, [this] {
      return !buffer_.empty() || this->is_closed();
    });
    if (buffer_.empty()) {
      throw std::runtime_error("receive on closed empty channel");
    }
    T value = std::move(buffer_.front());
    buffer_.pop_front();
    if constexpr (is_bounded) {
      send_cv_.notify_one();
      // lock released by RAII after notify
    }
    return value;
  }

  // Try to receive without blocking
  std::optional<T> try_receive() {
    typename base_type::lock_type lock(this->mutex_);
    if (buffer_.empty()) {
      return std::nullopt;
    }
    T value = std::move(buffer_.front());
    buffer_.pop_front();
    if constexpr (is_bounded) {
      send_cv_.notify_one();
      // lock released by RAII after notify
    }
    return value;
  }

  // Receive with timeout
  template <typename Rep, typename Period>
  channel_result<T> receive_for(std::chrono::duration<Rep, Period> timeout) {
    typename base_type::lock_type lock(this->mutex_);
    if (!this->wait_for_condition_for(lock, [this] {
          return !buffer_.empty() || this->is_closed();
        }, timeout)) {
      return {std::nullopt, channel_op_status::timeout};
    }
    if (buffer_.empty()) {
      return {std::nullopt, channel_op_status::closed};
    }
    T value = std::move(buffer_.front());
    buffer_.pop_front();
    if constexpr (is_bounded) {
      send_cv_.notify_one();
      // lock released by RAII after notify
    }
    return {std::move(value), channel_op_status::success};
  }

  // Check current buffer size
  std::size_t size() const {
    typename base_type::lock_type lock(this->mutex_);
    return buffer_.size();
  }

  // Check if empty
  bool empty() const {
    typename base_type::lock_type lock(this->mutex_);
    return buffer_.empty();
  }

  // Close is inherited from closeable_mixin
  using closeable::close;
  using closeable::is_closed;
};

// =============================================================================
// Async Channel Awaiters
// =============================================================================

template <typename T, typename BoundednessPolicy, typename LockPolicy>
class async_channel;

template <typename T, typename BoundednessPolicy, typename LockPolicy>
class channel_send_awaiter
    : public awaitable_base<channel_send_awaiter<T, BoundednessPolicy, LockPolicy>,
                            void> {
  async_channel<T, BoundednessPolicy, LockPolicy> &channel_;
  T value_;
  waiter_node node_;
  bool sent_{false};

public:
  channel_send_awaiter(async_channel<T, BoundednessPolicy, LockPolicy> &channel,
                       T value)
      : channel_(channel), value_(std::move(value)), node_(nullptr) {}

  bool ready_impl() noexcept {
    // Try to send immediately
    sent_ = channel_.try_send(std::move(value_));
    return sent_;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    node_.handle = h;
    channel_.add_send_waiter(&node_);
  }

  void resume_impl() {
    if (!sent_) {
      // We were woken, try to send now
      channel_.try_send(std::move(value_));
    }
  }
};

template <typename T, typename BoundednessPolicy, typename LockPolicy>
class channel_receive_awaiter
    : public awaitable_base<channel_receive_awaiter<T, BoundednessPolicy, LockPolicy>,
                            std::optional<T>> {
  async_channel<T, BoundednessPolicy, LockPolicy> &channel_;
  waiter_node node_;
  std::optional<T> result_;

public:
  explicit channel_receive_awaiter(
      async_channel<T, BoundednessPolicy, LockPolicy> &channel)
      : channel_(channel), node_(nullptr) {}

  bool ready_impl() noexcept {
    // Try to receive immediately
    result_ = channel_.try_receive();
    return result_.has_value() || channel_.is_closed();
  }

  void suspend_impl(std::coroutine_handle<> h) {
    node_.handle = h;
    channel_.add_receive_waiter(&node_);
  }

  std::optional<T> resume_impl() {
    if (!result_.has_value()) {
      // We were woken, try to receive now
      result_ = channel_.try_receive();
    }
    return std::move(result_);
  }
};

// =============================================================================
// Async Channel - Extends sync with coroutine-awaitable operations
// =============================================================================

template <typename T,
          typename BoundednessPolicy = unbounded_policy,
          typename LockPolicy = mutex_lock_policy>
class async_channel
    : public sync_channel<T, BoundednessPolicy, LockPolicy>,
      public async_primitive_base<async_channel<T, BoundednessPolicy, LockPolicy>> {
  using sync_base = sync_channel<T, BoundednessPolicy, LockPolicy>;
  using async_base =
      async_primitive_base<async_channel<T, BoundednessPolicy, LockPolicy>>;

  friend class channel_send_awaiter<T, BoundednessPolicy, LockPolicy>;
  friend class channel_receive_awaiter<T, BoundednessPolicy, LockPolicy>;

  // Separate waiter lists for senders and receivers
  std::atomic<waiter_node *> send_waiters_{nullptr};

  void add_send_waiter_internal(waiter_node *node) {
    waiter_node *old_head = send_waiters_.load(std::memory_order_acquire);
    do {
      node->next = old_head;
    } while (!send_waiters_.compare_exchange_weak(old_head, node,
                                                  std::memory_order_release,
                                                  std::memory_order_acquire));
  }

  void wake_one_sender() {
    waiter_node *head = send_waiters_.load(std::memory_order_acquire);
    while (head != nullptr) {
      if (send_waiters_.compare_exchange_weak(head, head->next,
                                              std::memory_order_release,
                                              std::memory_order_acquire)) {
        if (head->handle) {
          schedule_coro_handle(head->handle);
        }
        return;
      }
    }
  }

public:
  using value_type = T;
  using boundedness_policy = BoundednessPolicy;
  using lock_policy = LockPolicy;

  async_channel() = default;

  // Inherit sync API
  using sync_base::capacity;
  using sync_base::empty;
  using sync_base::is_bounded;
  using sync_base::is_closed;
  using sync_base::size;
  using sync_base::try_receive;
  using sync_base::try_send;

  // Override send to wake async receivers
  void send(T value) {
    sync_base::send(std::move(value));
    this->wake_one(); // Wake a receiver
  }

  // Override receive to wake async senders (for bounded channels)
  T receive() {
    T value = sync_base::receive();
    if constexpr (is_bounded) {
      wake_one_sender();
    }
    return value;
  }

  // Override close to wake all waiters
  void close() {
    sync_base::close();
    this->wake_all();      // Wake all receivers
    // Wake all senders
    waiter_node *head =
        send_waiters_.exchange(nullptr, std::memory_order_acq_rel);
    while (head != nullptr) {
      waiter_node *next = head->next;
      if (head->handle) {
        schedule_coro_handle(head->handle);
      }
      head = next;
    }
  }

  // Async send - returns awaiter
  auto send_async(T value) {
    return channel_send_awaiter<T, BoundednessPolicy, LockPolicy>(
        *this, std::move(value));
  }

  // Async receive - returns awaiter
  auto receive_async() {
    return channel_receive_awaiter<T, BoundednessPolicy, LockPolicy>(*this);
  }

  // Add waiter for send notifications
  void add_send_waiter(waiter_node *node) { add_send_waiter_internal(node); }

  // Add waiter for receive notifications
  void add_receive_waiter(waiter_node *node) { this->add_waiter(node); }
};

// =============================================================================
// Type Aliases
// =============================================================================

template <typename T>
using channel = sync_channel<T, unbounded_policy, mutex_lock_policy>;

template <typename T, std::size_t N>
using bounded_channel = sync_channel<T, bounded_policy<N>, mutex_lock_policy>;

template <typename T>
using async_unbounded_channel = async_channel<T, unbounded_policy, mutex_lock_policy>;

template <typename T, std::size_t N>
using async_bounded_channel = async_channel<T, bounded_policy<N>, mutex_lock_policy>;

// Convenience aliases
template <typename T>
using mpsc_channel = channel<T>; // Multiple producer, single consumer

template <typename T>
using spmc_channel = channel<T>; // Single producer, multiple consumer

template <typename T>
using mpmc_channel = channel<T>; // Multiple producer, multiple consumer

} // namespace ethreads

#endif // ETHREADS_SHARED_STATE_CHANNEL_HPP
