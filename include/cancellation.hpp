#ifndef ETHREADS_CANCELLATION_HPP
#define ETHREADS_CANCELLATION_HPP

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "shared_state/crtp_base.hpp"

namespace ethreads {

// Exception thrown when a cancelled operation is detected
struct cancelled_exception : std::exception {
  const char *what() const noexcept override { return "operation cancelled"; }
};

// Shared state for cancellation — one per cancellation_source
class cancellation_awaiter;

class cancellation_state : public async_primitive_base<cancellation_state> {
  friend class cancellation_awaiter;

public:
  cancellation_state() = default;

  bool is_cancelled() const {
    return cancelled_.load(std::memory_order_acquire);
  }

  void cancel() {
    if (cancelled_.exchange(true, std::memory_order_acq_rel))
      return; // already cancelled

    // Fire registered callbacks
    std::vector<std::function<void()>> cbs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cbs.swap(callbacks_);
    }
    for (auto &cb : cbs)
      cb();

    // Wake all coroutines awaiting cancellation
    wake_all();
  }

  // Register a callback invoked on cancellation. Returns an id for removal.
  // If already cancelled, fires immediately and returns 0.
  std::size_t register_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.load(std::memory_order_acquire)) {
      cb();
      return 0;
    }
    std::size_t id = next_id_++;
    callbacks_.push_back(std::move(cb));
    callback_ids_.push_back(id);
    return id;
  }

  void unregister_callback(std::size_t id) {
    if (id == 0)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < callback_ids_.size(); ++i) {
      if (callback_ids_[i] == id) {
        callbacks_.erase(callbacks_.begin() + static_cast<std::ptrdiff_t>(i));
        callback_ids_.erase(callback_ids_.begin() +
                            static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::vector<std::function<void()>> callbacks_;
  std::vector<std::size_t> callback_ids_;
  std::size_t next_id_{1};
};

// Awaiter returned by cancellation_token::cancelled()
class cancellation_awaiter
    : public awaitable_base<cancellation_awaiter, void> {
public:
  explicit cancellation_awaiter(std::shared_ptr<cancellation_state> state)
      : state_(std::move(state)) {}

  bool ready_impl() const { return state_->is_cancelled(); }

  void suspend_impl(std::coroutine_handle<> h) {
    node_ = waiter_node(h);
    state_->add_waiter(&node_);
    // Re-check after adding waiter to avoid missed wakeup
    if (state_->is_cancelled()) {
      // Try to wake ourselves — wake_all will schedule us
      state_->wake_all();
    }
  }

  void resume_impl() {
    if (!state_->is_cancelled())
      return; // spurious wakeup guard — shouldn't happen
  }

private:
  std::shared_ptr<cancellation_state> state_;
  waiter_node node_{nullptr};
};

// Lightweight copyable handle — does not own the state
class cancellation_token {
public:
  cancellation_token() = default;

  bool is_cancelled() const {
    return state_ && state_->is_cancelled();
  }

  void throw_if_cancelled() const {
    if (is_cancelled())
      throw cancelled_exception{};
  }

  // co_await token.cancelled() — suspends until cancellation
  cancellation_awaiter cancelled() const {
    return cancellation_awaiter{state_};
  }

  explicit operator bool() const { return state_ != nullptr; }

  // Access to state for sleep/IO integration
  std::shared_ptr<cancellation_state> state() const { return state_; }

private:
  friend class cancellation_source;
  explicit cancellation_token(std::shared_ptr<cancellation_state> state)
      : state_(std::move(state)) {}

  std::shared_ptr<cancellation_state> state_;
};

// Owns the cancellation state, creates tokens, triggers cancellation
class cancellation_source {
public:
  cancellation_source()
      : state_(std::make_shared<cancellation_state>()) {}

  cancellation_token token() const {
    return cancellation_token{state_};
  }

  void cancel() { state_->cancel(); }

  bool is_cancelled() const { return state_->is_cancelled(); }

private:
  std::shared_ptr<cancellation_state> state_;
};

} // namespace ethreads

#endif // ETHREADS_CANCELLATION_HPP
