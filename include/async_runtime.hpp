#ifndef ETHREADS_ASYNC_RUNTIME_HPP
#define ETHREADS_ASYNC_RUNTIME_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "task_scheduler.hpp"
#include "coro_task.hpp"
#include "coro_awaiter.hpp"
#include "cancellation.hpp"
#include "sleep.hpp"
#include "yield.hpp"
#include "select.hpp"

namespace ethreads {

// =============================================================================
// Blocking Result for cancellable run_blocking
// =============================================================================

enum class blocking_status { completed, cancelled };

template <typename T> struct blocking_result {
  blocking_status status;
  std::optional<T> value;
  std::exception_ptr exception;

  T get() {
    if (exception)
      std::rethrow_exception(exception);
    if (status == blocking_status::cancelled)
      throw cancelled_exception{};
    return std::move(*value);
  }
};

template <> struct blocking_result<void> {
  blocking_status status;
  std::exception_ptr exception;

  void get() {
    if (exception)
      std::rethrow_exception(exception);
    if (status == blocking_status::cancelled)
      throw cancelled_exception{};
  }
};

// =============================================================================
// Cancellable Blocking Awaiter
// =============================================================================

template <typename Result, typename Callable, typename... Args>
class cancellable_blocking_awaiter {
  struct shared_data {
    std::atomic<bool> resumed{false};
    std::coroutine_handle<> handle{nullptr};
    std::optional<Result> result;
    std::exception_ptr exception;
    blocking_status status{blocking_status::completed};
  };

public:
  cancellable_blocking_awaiter(cancellation_token token, Callable &&c,
                                Args &&...a)
      : token_(std::move(token)), callable_(std::forward<Callable>(c)),
        args_(std::forward<Args>(a)...),
        data_(std::make_shared<shared_data>()) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    data_->handle = h;

    auto data = data_;
    auto token = token_;

    // Register cancellation callback
    if (auto state = token.state()) {
      callback_id_ = state->register_callback([data]() {
        bool expected = false;
        if (data->resumed.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
          data->status = blocking_status::cancelled;
          schedule_coro_handle(data->handle);
        }
      });
    }

    // Schedule work on pool
    auto task = [data, callable = callable_, args = args_]() mutable {
      try {
        if constexpr (std::is_void_v<Result>) {
          std::apply(callable, std::move(args));
        } else {
          data->result = std::apply(callable, std::move(args));
        }
      } catch (...) {
        data->exception = std::current_exception();
      }

      bool expected = false;
      if (data->resumed.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
        data->status = blocking_status::completed;
        schedule_coro_handle(data->handle);
      }
    };

    schedule_task_on_pool(std::move(task));
  }

  blocking_result<Result> await_resume() {
    // Unregister callback
    if (auto state = token_.state()) {
      if (callback_id_ != 0)
        state->unregister_callback(callback_id_);
    }

    return blocking_result<Result>{data_->status, std::move(data_->result),
                                    data_->exception};
  }

private:
  cancellation_token token_;
  Callable callable_;
  std::tuple<Args...> args_;
  std::shared_ptr<shared_data> data_;
  std::size_t callback_id_{0};
};

// Void specialization
template <typename Callable, typename... Args>
class cancellable_blocking_awaiter<void, Callable, Args...> {
  struct shared_data {
    std::atomic<bool> resumed{false};
    std::coroutine_handle<> handle{nullptr};
    std::exception_ptr exception;
    blocking_status status{blocking_status::completed};
  };

public:
  cancellable_blocking_awaiter(cancellation_token token, Callable &&c,
                                Args &&...a)
      : token_(std::move(token)), callable_(std::forward<Callable>(c)),
        args_(std::forward<Args>(a)...),
        data_(std::make_shared<shared_data>()) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    data_->handle = h;

    auto data = data_;
    auto token = token_;

    if (auto state = token.state()) {
      callback_id_ = state->register_callback([data]() {
        bool expected = false;
        if (data->resumed.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
          data->status = blocking_status::cancelled;
          schedule_coro_handle(data->handle);
        }
      });
    }

    auto task = [data, callable = callable_, args = args_]() mutable {
      try {
        std::apply(callable, std::move(args));
      } catch (...) {
        data->exception = std::current_exception();
      }

      bool expected = false;
      if (data->resumed.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
        data->status = blocking_status::completed;
        schedule_coro_handle(data->handle);
      }
    };

    schedule_task_on_pool(std::move(task));
  }

  blocking_result<void> await_resume() {
    if (auto state = token_.state()) {
      if (callback_id_ != 0)
        state->unregister_callback(callback_id_);
    }

    return blocking_result<void>{data_->status, data_->exception};
  }

private:
  cancellation_token token_;
  Callable callable_;
  std::tuple<Args...> args_;
  std::shared_ptr<shared_data> data_;
  std::size_t callback_id_{0};
};

// =============================================================================
// Async Runtime - Coroutine-first execution environment
// =============================================================================

class async_runtime {
public:
  // Run a single coroutine to completion, return its result
  template <typename T> T run(coro_task<T> task) {
    task.start();
    return task.get();
  }

  // Specialization for void
  void run(coro_task<void> task) {
    task.start();
    task.get();
  }

  // Run a coroutine that returns int (for coro_main)
  int run_main(coro_task<int> task) { return run(std::move(task)); }

  // Block on a coroutine from non-coroutine context
  template <typename T> T block_on(coro_task<T> task) {
    return run(std::move(task));
  }

  // Spawn and detach (fire-and-forget)
  // Uses shared_ptr to prevent memory leaks
  template <typename T> void spawn_detached(coro_task<T> task) {
    auto shared_task = std::make_shared<coro_task<T>>(std::move(task));
    shared_task->start();
    // Task will be cleaned up when it completes and shared_ptr ref count drops
    // Store in detached list to prevent premature destruction
    std::lock_guard<std::mutex> lock(detached_mutex_);

    // Auto-cleanup if list gets large (prevents unbounded growth)
    if (detached_tasks_.size() > 100) {
      detached_tasks_.erase(
          std::remove_if(detached_tasks_.begin(), detached_tasks_.end(),
                         [](auto &check) { return check(); }),
          detached_tasks_.end());
    }

    detached_tasks_.push_back([shared_task]() mutable {
      if (shared_task->is_ready()) {
        shared_task.reset();
        return true;
      }
      return false;
    });
  }

  // Clean up completed detached tasks (call periodically if needed)
  void collect_detached() {
    std::lock_guard<std::mutex> lock(detached_mutex_);
    detached_tasks_.erase(
        std::remove_if(detached_tasks_.begin(), detached_tasks_.end(),
                       [](auto &check) { return check(); }),
        detached_tasks_.end());
  }

  // Run a blocking callable on the thread pool, resume coroutine when done
  template <typename Callable, typename... Args>
  auto run_blocking(Callable &&callable, Args &&...args) {
    return make_awaiter(std::forward<Callable>(callable),
                        std::forward<Args>(args)...);
  }

  // Cancellable variant: resumes coroutine early on cancellation.
  // Pool thread continues to completion (result discarded).
  template <typename Callable, typename... Args>
  auto run_blocking(cancellation_token token, Callable &&callable,
                    Args &&...args) {
    using result_type = std::invoke_result_t<Callable, Args...>;
    return cancellable_blocking_awaiter<result_type, std::decay_t<Callable>,
                                        std::decay_t<Args>...>(
        std::move(token), std::forward<Callable>(callable),
        std::forward<Args>(args)...);
  }

private:
  std::mutex detached_mutex_;
  std::vector<std::function<bool()>> detached_tasks_;
};

// Global runtime instance
extern async_runtime g_runtime;

// =============================================================================
// Structured Concurrency: when_all
// =============================================================================

// Wait for all tasks to complete, return vector of results
template <typename T>
coro_task<std::vector<T>> when_all(std::vector<coro_task<T>> tasks) {
  // Start all tasks
  for (auto &t : tasks) {
    t.start();
  }

  // Await all results in order
  std::vector<T> results;
  results.reserve(tasks.size());
  for (auto &t : tasks) {
    results.push_back(co_await t);
  }

  co_return results;
}

// Specialization for void tasks
inline coro_task<void> when_all(std::vector<coro_task<void>> tasks) {
  // Start all tasks
  for (auto &t : tasks) {
    t.start();
  }

  // Await all
  for (auto &t : tasks) {
    co_await t;
  }

  co_return;
}

// =============================================================================
// Structured Concurrency: when_any
// =============================================================================

// Result type for when_any - includes index and value
template <typename T> struct when_any_result {
  std::size_t index;
  T value;
};

// Wait for first task to complete, return its result and index
template <typename T>
coro_task<when_any_result<T>> when_any(std::vector<coro_task<T>> tasks) {
  if (tasks.empty()) {
    throw std::invalid_argument("when_any called with empty task list");
  }

  // Start all tasks
  for (auto &t : tasks) {
    t.start();
  }

  // Poll until one is ready
  while (true) {
    for (std::size_t i = 0; i < tasks.size(); ++i) {
      if (tasks[i].is_ready()) {
        co_return when_any_result<T>{i, co_await tasks[i]};
      }
    }
    // Yield to let workers make progress
    co_await yield();
  }
}

// Specialization for void - just return the index
inline coro_task<std::size_t> when_any(std::vector<coro_task<void>> tasks) {
  if (tasks.empty()) {
    throw std::invalid_argument("when_any called with empty task list");
  }

  // Start all tasks
  for (auto &t : tasks) {
    t.start();
  }

  // Poll until one is ready
  while (true) {
    for (std::size_t i = 0; i < tasks.size(); ++i) {
      if (tasks[i].is_ready()) {
        co_await tasks[i];
        co_return i;
      }
    }
    co_await yield();
  }
}

} // namespace ethreads

// =============================================================================
// Entry Point Macro
// =============================================================================

// User defines: ethreads::coro_task<int> coro_main() { ... }
// Then uses CORO_MAIN to generate the actual main()

#define CORO_MAIN                                                              \
  ethreads::coro_task<int> coro_main();                                        \
  int main() { return ethreads::g_runtime.run_main(coro_main()); }

#endif
