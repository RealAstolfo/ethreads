#include <atomic>
#include <iostream>
#include <memory>
#include <stddef.h>
#include <thread>

#include "ts_queue.hpp"
#include "task_scheduler.hpp"

task_scheduler g_global_task_scheduler;
thread_local std::size_t task_scheduler::cacheline_size;

#ifdef _WIN32
#ifdef __MINGW32__
#warning "Unsupported target: Cache line size may not be accurately determined."
inline std::size_t get_cache_line_size() {
  return 64; // Default value for unsupported platforms or architectures
}
#elif
#include <Windows.h>
inline std::size_t get_cache_line_size() {
  DWORD buffer_size = 0;
  DWORD cache_line_size = 0;
  DWORD buffer[4] = {0};

  if (GetLogicalProcessorInformation(buffer, &buffer_size) && buffer_size > 0) {
    const DWORD num_elements =
        buffer_size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    const SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info =
        reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION *>(buffer);

    for (DWORD i = 0; i < num_elements; ++i) {
      if (info[i].Relationship == RelationCache && info[i].Cache.Level == 1) {
        cache_line_size = info[i].Cache.LineSize;
        break;
      }
    }
  }

  if (cache_line_size == 0)
    cache_line_size = 64; // Default value for x86 and x86-64 architectures

  return cache_line_size;
}
#endif
#elif defined(__linux__)
#include <unistd.h>
inline std::size_t get_cache_line_size() {
  long result = 0;//sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  if (result <= 0)
    result = 64; // Default value for x86 and x86-64 architectures

  return static_cast<std::size_t>(result);
}
#else
#warning "Unsupported target: Cache line size may not be accurately determined."
inline std::size_t get_cache_line_size() {
  return 64; // Default value for unsupported platforms or architectures
}
#endif

int worker(std::shared_ptr<ts_queue<task>> task_queue) {
  task_scheduler::cacheline_size =
      get_cache_line_size(); // cacheline_size is thread_local, so we have to
                             // initialize it for all threads, that way
                             // add_batch_tasks works properly when called by
                             // non-main
  while (!task_queue.unique()) {
    if (auto task = task_queue->pop())
      (*task)();
  }

  return 0;
}

task_scheduler::task_scheduler() {
  size_t processor_count = std::thread::hardware_concurrency();
  while (processor_count-- > 1) {
    std::shared_ptr<ts_queue<task>> tsq = std::make_shared<ts_queue<task>>();
    std::thread thr(worker, tsq);
    workers.push_back(std::make_pair(std::move(thr), std::move(tsq)));
  }

  cacheline_size = get_cache_line_size();
}

task_scheduler::~task_scheduler() {
  for (auto &thr : workers) {
    thr.second->push(
        []() { return 0; }); // Add an empty task to wake up the
                             // threads, for their demise of course.
  }

  for (auto &thr : workers)
    thr.first.detach();
}
