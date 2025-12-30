#ifndef ETHREADS_ASYNC_RUNTIME_HPP
#define ETHREADS_ASYNC_RUNTIME_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "coro_task.hpp"
#include "yield.hpp"

namespace ethreads {

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
