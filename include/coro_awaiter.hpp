#ifndef ETHREADS_CORO_AWAITER_HPP
#define ETHREADS_CORO_AWAITER_HPP

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ethreads {

// Forward declarations
void schedule_coro_handle(std::coroutine_handle<> handle);
void schedule_task_on_pool(std::function<void()> task);

// Awaiter that schedules a callable on the thread pool
// and resumes the coroutine when complete
template <typename Callable, typename... Args> class callable_awaiter {
public:
  using result_type = std::invoke_result_t<Callable, Args...>;

  callable_awaiter(Callable &&c, Args &&...a)
      : callable_(std::forward<Callable>(c)),
        args_(std::forward<Args>(a)...) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    continuation_ = h;

    auto task = [this]() {
      try {
        if constexpr (std::is_void_v<result_type>) {
          std::apply(callable_, std::move(args_));
        } else {
          result_ = std::apply(callable_, std::move(args_));
        }
      } catch (...) {
        exception_ = std::current_exception();
      }
      done_.store(true, std::memory_order_release);
      schedule_coro_handle(continuation_);
    };

    schedule_task_on_pool(std::move(task));
  }

  result_type await_resume() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    if constexpr (!std::is_void_v<result_type>) {
      return std::move(*result_);
    }
  }

private:
  Callable callable_;
  std::tuple<Args...> args_;
  std::optional<result_type> result_;
  std::exception_ptr exception_;
  std::atomic<bool> done_{false};
  std::coroutine_handle<> continuation_;
};

// Specialization for void result
template <typename Callable, typename... Args>
  requires std::is_void_v<std::invoke_result_t<Callable, Args...>>
class callable_awaiter<Callable, Args...> {
public:
  callable_awaiter(Callable &&c, Args &&...a)
      : callable_(std::forward<Callable>(c)),
        args_(std::forward<Args>(a)...) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    continuation_ = h;

    auto task = [this]() {
      try {
        std::apply(callable_, std::move(args_));
      } catch (...) {
        exception_ = std::current_exception();
      }
      done_.store(true, std::memory_order_release);
      schedule_coro_handle(continuation_);
    };

    schedule_task_on_pool(std::move(task));
  }

  void await_resume() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

private:
  Callable callable_;
  std::tuple<Args...> args_;
  std::exception_ptr exception_;
  std::atomic<bool> done_{false};
  std::coroutine_handle<> continuation_;
};

// Factory function for creating awaiters
template <typename Callable, typename... Args>
auto make_awaiter(Callable &&callable, Args &&...args) {
  return callable_awaiter<std::decay_t<Callable>, std::decay_t<Args>...>(
      std::forward<Callable>(callable), std::forward<Args>(args)...);
}

} // namespace ethreads

#endif
