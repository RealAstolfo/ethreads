#ifndef ETHREADS_TASK_SCHEDULER_HPP
#define ETHREADS_TASK_SCHEDULER_HPP

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coro_task.hpp"
#include "task.hpp"
#include "ts_queue.hpp"
#include "ws_deque.hpp"

/*
  The Userspace Threads "Fibers" rely on kernel threads, but avoid the expensive
  context switching of user to kernel and back, replacing it with a much more
  lightweight context switch. the logic of the task scheduler is minimal, it
  should be implementation defined. a good start is just having a three queue
  system to ensure critical routines that must be completed each frame as high.
  background things such as level loading or other slower processes from the OS
  in normal rather slow by computer standards have all the networking actions
  contained within low priority
*/

struct worker_info {
  std::thread thread;
  std::shared_ptr<ts_queue<task>> queue;
  std::chrono::high_resolution_clock::time_point available_at;
};

struct task_scheduler;

// Coroutine worker info for work-stealing scheduler
struct coro_worker_info {
  std::thread thread;
  std::shared_ptr<ethreads::ws_deque<std::coroutine_handle<>>> coro_queue;
  std::size_t worker_id;
  task_scheduler *scheduler;
};

struct task_scheduler {
  // Existing task workers
  std::vector<worker_info> workers;
  std::unordered_map<std::uintptr_t, std::chrono::nanoseconds> task_durations;
  std::mutex access_mutex;
  std::atomic<bool> shutting_down{false};

  // Coroutine workers with work-stealing
  std::vector<coro_worker_info> coro_workers;
  std::atomic<std::size_t> coro_worker_index{0};

  // Global queue for external coroutine submissions (MPMC-safe)
  std::shared_ptr<ts_queue<std::coroutine_handle<>>> external_coro_queue;

  // Thread-local state for coroutine workers
  static thread_local std::size_t cacheline_size;
  static thread_local std::size_t current_coro_worker_id;
  static thread_local bool is_coro_worker;

  // Existing task API
  template <typename T, typename... Args>
  inline std::future<std::invoke_result_t<T, Args...>> add_task(T &&,
                                                                Args &&...);

  template <typename T, typename Iterator, typename... Args>
  std::vector<std::future<std::invoke_result_t<T, Iterator, Iterator, Args...>>>
  add_batch_task(T &&t, Iterator &&start, Iterator &&stop, Args &&...args);

  // Coroutine API
  template <typename T>
  ethreads::coro_task<T> add_coro(ethreads::coro_task<T> task);

  template <typename Callable, typename... Args>
    requires(!ethreads::is_coro_task_v<std::invoke_result_t<Callable, Args...>>)
  auto add_coro(Callable &&callable, Args &&...args)
      -> ethreads::coro_task<std::invoke_result_t<Callable, Args...>>;

  // Schedule a coroutine handle on a worker
  void schedule_coro_handle(std::coroutine_handle<> handle);

  // Schedule a task on the regular task pool
  void schedule_task(std::function<void()> task);

  task_scheduler();
  ~task_scheduler();
};

extern task_scheduler g_global_task_scheduler;

template <typename T, typename... Args>
std::future<std::invoke_result_t<T, Args...>> inline add_task(T &&t,
                                                              Args &&...args) {
  return g_global_task_scheduler.add_task(std::forward<T>(t),
                                          std::forward<Args>(args)...);
}

template <typename T, typename Iterator, typename... Args>
std::vector<std::future<std::invoke_result_t<
    T, Iterator, Iterator, Args...>>> inline add_batch_task(T &&t,
                                                            Iterator &&start,
                                                            Iterator &&stop,
                                                            Args... args) {
  return g_global_task_scheduler.add_batch_task(
      std::forward<T>(t), std::forward<Iterator>(start),
      std::forward<Iterator>(stop), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
std::future<std::invoke_result_t<T, Args...>>
task_scheduler::add_task(T &&t, Args &&...args) {
  using result_type = std::invoke_result_t<T, Args...>;
  using nanosec = std::chrono::duration<std::size_t, std::nano>;

  auto promise = std::make_shared<std::promise<result_type>>();
  std::future<result_type> future = promise->get_future();

  std::function<result_type()> func =
      std::bind(std::forward<T>(t), std::forward<Args>(args)...);
  std::chrono::high_resolution_clock::duration task_duration =
      std::chrono::high_resolution_clock::duration::zero();

  const std::uintptr_t func_ptr = reinterpret_cast<std::uintptr_t>(&t);
  if (auto it = task_durations.find(func_ptr); it != task_durations.end()) {
    task_duration = it->second;
  }

  std::function<void()> wrapped_task =
      [promise, func, func_ptr, task_durations = std::ref(this->task_durations),
       access_mutex = std::ref(this->access_mutex)]() mutable {
        auto start = std::chrono::high_resolution_clock::now();
        try {
          if constexpr (std::is_void_v<result_type>) {
            func();
            promise->set_value();
          } else {
            promise->set_value(func());
          }
        } catch (...) {
          promise->set_exception(std::current_exception());
        }

        auto stop = std::chrono::high_resolution_clock::now();
        nanosec duration = stop - start;
        {
          std::lock_guard<std::mutex> lock(access_mutex.get());
          task_durations.get()[func_ptr] = duration;
        }
      };

  auto now = std::chrono::high_resolution_clock::now();
  {
    std::lock_guard<std::mutex> lock(access_mutex);
    if (workers.empty()) {
      return future;
    }

    worker_info *least_busy = &workers[0];
    for (auto &worker : workers) {
      if (now >= worker.available_at) {
        worker.queue->push(wrapped_task);
        worker.available_at = now + task_duration;
        return future;
      }

      if (least_busy->available_at > worker.available_at)
        least_busy = &worker;
    }

    least_busy->queue->push(wrapped_task);
    least_busy->available_at = now + task_duration;
  }

  return future;
}

template <typename T, typename Iterator, typename... Args>
std::vector<std::future<std::invoke_result_t<T, Iterator, Iterator, Args...>>>
task_scheduler::add_batch_task(T &&t, Iterator &&start, Iterator &&stop,
                               Args &&...args) {
  using result_type = std::invoke_result_t<T, Iterator, Iterator, Args...>;
  const std::size_t total_items = std::distance(start, stop);
  const std::size_t num_threads = std::thread::hardware_concurrency();
  const std::size_t cache_line_size = cacheline_size;

  std::vector<std::future<result_type>> futures;
  if (total_items == 0 || num_threads > total_items) {
    futures.push_back(add_task(t, start, stop, args...));
    return futures;
  }

  futures.reserve(num_threads);

  const std::size_t aligned_batch_size =
      (total_items + num_threads - 1) / num_threads;
  const std::size_t aligned_batch_size_bytes =
      aligned_batch_size * sizeof(*start);
  const std::size_t aligned_batch_size_elements =
      std::max(1ul, aligned_batch_size_bytes / cache_line_size);
  const std::size_t num_aligned_batches =
      total_items / aligned_batch_size_elements;

  Iterator batch_start = start;

  for (std::size_t i = 0; i < num_aligned_batches; ++i) {
    Iterator batch_stop = batch_start;
    std::advance(batch_stop, aligned_batch_size_elements);
    futures.push_back(add_task(t, batch_start, batch_stop, args...));
    batch_start = batch_stop;
  }

  futures.push_back(add_task(t, batch_start, stop, args...));

  return futures;
}

// =============================================================================
// Coroutine API Implementation
// =============================================================================

// Schedule an existing coro_task on workers (eagerly starts execution)
template <typename T>
ethreads::coro_task<T> task_scheduler::add_coro(ethreads::coro_task<T> task) {
  // Mark as scheduled and start execution
  task.start();
  return std::move(task);
}

// Wrap a regular callable in a coroutine and schedule it
template <typename Callable, typename... Args>
  requires(!ethreads::is_coro_task_v<std::invoke_result_t<Callable, Args...>>)
auto task_scheduler::add_coro(Callable &&callable, Args &&...args)
    -> ethreads::coro_task<std::invoke_result_t<Callable, Args...>> {
  using result_type = std::invoke_result_t<Callable, Args...>;

  // Create a coroutine that wraps the callable
  auto wrapper = [](Callable c, Args... a) -> ethreads::coro_task<result_type> {
    if constexpr (std::is_void_v<result_type>) {
      c(std::forward<Args>(a)...);
      co_return;
    } else {
      co_return c(std::forward<Args>(a)...);
    }
  };

  auto task = wrapper(std::forward<Callable>(callable),
                      std::forward<Args>(args)...);
  task.start();
  return task;
}

// =============================================================================
// Free Function Wrappers for Coroutine API
// =============================================================================

template <typename T>
ethreads::coro_task<T> add_coro(ethreads::coro_task<T> task) {
  return g_global_task_scheduler.add_coro(std::move(task));
}

template <typename Callable, typename... Args>
  requires(!ethreads::is_coro_task_v<std::invoke_result_t<Callable, Args...>>)
auto add_coro(Callable &&callable, Args &&...args) {
  return g_global_task_scheduler.add_coro(std::forward<Callable>(callable),
                                          std::forward<Args>(args)...);
}

// =============================================================================
// Implementation for ethreads namespace functions (called from coro_task.hpp)
// =============================================================================

namespace ethreads {

inline void schedule_coro_handle(std::coroutine_handle<> handle) {
  g_global_task_scheduler.schedule_coro_handle(handle);
}

inline void schedule_task_on_pool(std::function<void()> task) {
  g_global_task_scheduler.schedule_task(std::move(task));
}

} // namespace ethreads

#endif
