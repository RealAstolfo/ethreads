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

class sleep_awaiter : public awaitable_base<sleep_awaiter, sleep_status> {
public:
  sleep_awaiter(std::chrono::steady_clock::time_point deadline,
                cancellation_token token = {})
      : deadline_(deadline), token_(std::move(token)) {}

  bool ready_impl() const {
    if (token_.is_cancelled())
      return true;
    return std::chrono::steady_clock::now() >= deadline_;
  }

  void suspend_impl(std::coroutine_handle<> h) {
    handle_ = h;
    fired_.store(false, std::memory_order_relaxed);

    // Register with timer service
    auto cancel_state = token_.state();
    get_timer_service().add_timer(deadline_, h, cancel_state, &fired_);

    // Register cancellation callback if token is valid
    if (cancel_state) {
      callback_id_ = cancel_state->register_callback([this]() {
        // Race with timer expiry — exactly one wins via fired_ flag
        bool expected = false;
        if (fired_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
          schedule_coro_handle(handle_);
        }
      });
    }
  }

  sleep_status resume_impl() {
    // Unregister cancellation callback to avoid dangling reference
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
  std::coroutine_handle<> handle_;
  std::atomic<bool> fired_{false};
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
