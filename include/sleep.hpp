#ifndef ETHREADS_SLEEP_HPP
#define ETHREADS_SLEEP_HPP

#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>

#include "cancellation.hpp"
#include "timer_service.hpp"
#include "shared_state/crtp_base.hpp"

namespace ethreads {

enum class sleep_status { completed, cancelled };

// Shared state between sleep_awaiter, the timer_service entry, and the
// cancellation callback. Heap-allocated via shared_ptr so the timer can
// still check `fired` after the awaiter is destroyed (cancellation wakes
// the coroutine before the deadline — awaiter goes out of scope, but the
// timer entry for the original deadline is still in the heap).
struct sleep_waiter_state {
  std::coroutine_handle<> handle;
  std::atomic<bool> fired{false};
};

class sleep_awaiter : public awaitable_base<sleep_awaiter, sleep_status> {
public:
  sleep_awaiter(std::chrono::steady_clock::time_point deadline,
                cancellation_token token = {})
      : deadline_(deadline), token_(std::move(token)),
        state_(std::make_shared<sleep_waiter_state>()) {}

  bool ready_impl() const {
    if (token_.is_cancelled())
      return true;
    return std::chrono::steady_clock::now() >= deadline_;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    state_->handle = h;
    state_->fired.store(false, std::memory_order_relaxed);

    // Register with timer service — hands timer service a shared_ptr to
    // state_ so the fired flag survives the awaiter's destruction.
    auto cancel_state = token_.state();
    get_timer_service().add_timer(deadline_, cancel_state, state_);

    // Register cancellation callback if token is valid. Capture state_ by
    // value (shared_ptr copy) so the callback is safe to fire even after
    // the awaiter is gone.
    if (cancel_state) {
      auto s = state_;
      callback_id_ = cancel_state->register_callback([s]() {
        // Race with timer expiry — exactly one wins via fired flag
        bool expected = false;
        if (s->fired.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
          schedule_coro_handle(s->handle);
        }
      });
    }
  }

  sleep_status resume_impl() {
    // Unregister cancellation callback to avoid invoking it after the
    // awaiter returns (and keep state_ from being leaked via the closure).
    if (auto state = token_.state()) {
      if (callback_id_ != 0)
        state->unregister_callback(callback_id_);
    }

    if (token_.is_cancelled())
      return sleep_status::cancelled;
    return sleep_status::completed;
  }

private:
  std::chrono::steady_clock::time_point deadline_;
  cancellation_token token_;
  std::shared_ptr<sleep_waiter_state> state_;
  std::size_t callback_id_{0};
};

// Sleep for a duration, optionally cancellable
template <typename Rep, typename Period>
sleep_awaiter sleep(std::chrono::duration<Rep, Period> duration,
                    cancellation_token token = {}) {
  return sleep_awaiter(std::chrono::steady_clock::now() + duration,
                       std::move(token));
}

// Sleep until a time point, optionally cancellable
inline sleep_awaiter
sleep_until(std::chrono::steady_clock::time_point deadline,
            cancellation_token token = {}) {
  return sleep_awaiter(deadline, std::move(token));
}

} // namespace ethreads

#endif // ETHREADS_SLEEP_HPP
