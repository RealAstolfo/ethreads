#include <atomic>
#include <chrono>
#include <memory>
#include <ratio>
#include <stddef.h>
#include <thread>

#include <mimalloc.h>

#include "task_scheduler.hpp"
#include "allocator.hpp"

constexpr std::size_t DEFAULT_CACHE_LINE_SIZE = 64;

task_scheduler g_global_task_scheduler;
thread_local std::size_t task_scheduler::cacheline_size;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <features.h>
#ifndef __USE_GNU
#define __MUSL__
#endif
#undef _GNU_SOURCE
#else
#include <features.h>
#ifndef __USE_GNU
#define __MUSL__
#endif
#endif

#ifdef _WIN32
#ifdef __MINGW32__
#warning "Unsupported target: Cache line size may not be accurately determined."
inline std::size_t get_cache_line_size() {
  return DEFAULT_CACHE_LINE_SIZE;
}
#else
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
    cache_line_size = DEFAULT_CACHE_LINE_SIZE;

  return cache_line_size;
}
#endif
#elif defined(__linux__)
#include <unistd.h>

inline std::size_t get_cache_line_size() {
#if defined(__MUSL__)
  return DEFAULT_CACHE_LINE_SIZE;
#else
  long result = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  if (result <= 0)
    result = DEFAULT_CACHE_LINE_SIZE;

  return static_cast<std::size_t>(result);
#endif
}
#else
#warning "Unsupported target: Cache line size may not be accurately determined."
inline std::size_t get_cache_line_size() {
  return DEFAULT_CACHE_LINE_SIZE;
}
#endif

void worker(std::shared_ptr<ethreads::channel<task>> task_queue,
            std::atomic<bool> &shutdown_flag) {
  mi_thread_init();
  task_scheduler::cacheline_size =
      get_cache_line_size(); // cacheline_size is thread_local, so we have to
                             // initialize it for all threads, that way
                             // add_batch_tasks works properly when called by
                             // non-main
  while (!shutdown_flag.load(std::memory_order_relaxed)) {
    // Use receive_for with timeout to periodically check shutdown flag
    auto result = task_queue->receive_for(std::chrono::milliseconds(100));
    if (result) {
      (*result)();
    } else if (task_queue->is_closed()) {
      break;
    }
  }
  mi_thread_done();
}

// Forward declarations from coro_scheduler.cpp
void init_coro_workers(task_scheduler &scheduler);
void shutdown_coro_workers(task_scheduler &scheduler);

// Forward declarations from timer_service.cpp
namespace ethreads {
void init_timer_service();
void shutdown_timer_service();
}

// Forward declarations from io_uring_service.cpp
namespace ethreads {
void init_io_uring_service();
void shutdown_io_uring_service();
}

task_scheduler::task_scheduler() {
  ethreads::init_allocator();
  size_t processor_count = std::thread::hardware_concurrency();
  while (processor_count-- > 0) {
    auto chan = std::make_shared<ethreads::channel<task>>();
    std::thread thr(worker, chan, std::ref(shutting_down));
    workers.push_back(worker_info{
        std::move(thr), std::move(chan),
        std::chrono::high_resolution_clock::now()});
  }

  cacheline_size = get_cache_line_size();

  // Initialize coroutine workers
  init_coro_workers(*this);

  // Initialize timer service
  ethreads::init_timer_service();

  // Initialize io_uring service
  ethreads::init_io_uring_service();
}

task_scheduler::~task_scheduler() {
  shutting_down.store(true, std::memory_order_relaxed);

  // Shutdown io_uring service (cancels in-flight I/O)
  ethreads::shutdown_io_uring_service();

  // Shutdown timer service (fires remaining timers)
  ethreads::shutdown_timer_service();

  // Shutdown coroutine workers
  shutdown_coro_workers(*this);

  // Shutdown regular task workers - close channels to wake up threads
  for (auto &w : workers) {
    w.queue->close();
  }

  for (auto &w : workers) {
    if (w.thread.joinable())
      w.thread.join();
  }
}
