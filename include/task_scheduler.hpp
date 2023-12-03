#ifndef ETHREADS_TASK_SCHEDULER
#define ETHREADS_TASK_SCHEDULER

#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "ts_queue.hpp"
#include "task.hpp"

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

using task = std::function<void()>;

struct task_scheduler {
  std::vector<std::pair<std::thread, std::shared_ptr<ts_queue<task>>>> workers;
  std::atomic_size_t last = 0;
  static thread_local std::size_t cacheline_size;

  template <typename T, typename... Args>
  inline std::future<std::invoke_result_t<T, Args...>> add_task(T &&,
                                                                Args &&...);

  template <typename T, typename Iterator, typename... Args>
  std::vector<std::future<std::invoke_result_t<T, Iterator, Iterator, Args...>>>
  add_batch_task(T &&t, Iterator &&start, Iterator &&stop, Args &&...args);

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
  auto promise = std::make_shared<std::promise<result_type>>();
  std::future<result_type> future = promise->get_future();
  std::function<void()> task =
      [promise, packaged = std::forward<T>(t),
       args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        std::apply(
            [&](auto &&...expandedArgs) {
              if constexpr (std::is_void_v<result_type>) {
                packaged(std::forward<decltype(expandedArgs)>(expandedArgs)...);
                promise->set_value();
              } else {
                promise->set_value(packaged(
                    std::forward<decltype(expandedArgs)>(expandedArgs)...));
              }
            },
            args);
      };
  workers[last++].second->push(task);
  if (last >= workers.size())
    last = 0;
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
      aligned_batch_size_bytes / cache_line_size;
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

#endif
