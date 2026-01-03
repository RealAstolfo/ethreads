#include <atomic>
#include <coroutine>
#include <random>
#include <thread>

#include "task_scheduler.hpp"

// Thread-local coroutine worker state
thread_local std::size_t task_scheduler::current_coro_worker_id = 0;
thread_local bool task_scheduler::is_coro_worker = false;

// Coroutine worker function with work-stealing
static void coro_worker(coro_worker_info &info) {
  task_scheduler::current_coro_worker_id = info.worker_id;
  task_scheduler::is_coro_worker = true;

  std::random_device rd;
  std::mt19937 rng(rd());

  auto &scheduler = *info.scheduler;
  auto &my_queue = *info.coro_queue;
  auto &external_queue = *scheduler.external_coro_queue;

  while (!scheduler.shutting_down.load(std::memory_order_acquire)) {
    // 1. Try to get work from own queue (LIFO - better cache locality)
    if (auto handle = my_queue.pop()) {
      // Validate handle before resuming
      if (*handle && handle->address() != nullptr &&
          *handle != std::noop_coroutine() && !handle->done()) {
        try {
          handle->resume();
        } catch (...) {
          // Swallow exceptions from coroutine - they should be handled via promise
        }
      }
      continue;
    }

    // 2. Check external queue (submissions from non-worker threads)
    if (auto handle = external_queue.try_receive()) {
      // Validate handle before resuming
      if (*handle && handle->address() != nullptr &&
          *handle != std::noop_coroutine() && !handle->done()) {
        try {
          handle->resume();
        } catch (...) {
          // Swallow exceptions from coroutine - they should be handled via promise
        }
      }
      continue;
    }

    // 3. Work-stealing: try to steal from other workers (FIFO - fairness)
    bool found_work = false;
    std::size_t num_workers = scheduler.coro_workers.size();

    if (num_workers > 1) {
      std::size_t start = rng() % num_workers;

      for (std::size_t i = 0; i < num_workers && !found_work; ++i) {
        std::size_t victim_id = (start + i) % num_workers;
        if (victim_id == info.worker_id)
          continue;

        auto &victim_queue = *scheduler.coro_workers[victim_id].coro_queue;
        if (auto stolen = victim_queue.steal()) {
          // Validate handle before resuming
          if (*stolen && stolen->address() != nullptr &&
              *stolen != std::noop_coroutine() && !stolen->done()) {
            try {
              stolen->resume();
            } catch (...) {
              // Swallow exceptions from coroutine - they should be handled via promise
            }
            found_work = true;
          }
        }
      }
    }

    // 4. If no work found, block on condition variable until notified
    if (!found_work) {
      std::unique_lock<std::mutex> lock(scheduler.coro_work_mutex);
      scheduler.coro_work_cv.wait(lock);
      // After waking (from work submission or shutdown), continue loop
    }
  }
}

// Schedule a coroutine handle on coroutine workers
void task_scheduler::schedule_coro_handle(std::coroutine_handle<> handle) {
  // Validate handle
  if (!handle || handle.address() == nullptr || handle.done()) {
    return;
  }

  // Extra check for obviously invalid addresses
  uintptr_t addr = reinterpret_cast<uintptr_t>(handle.address());
  if (addr < 0x1000) {  // Likely null/garbage pointer
    return;
  }

  // Don't schedule if shutting down
  if (shutting_down.load(std::memory_order_acquire)) {
    // Run inline during shutdown
    if (!handle.done()) {
      try {
        handle.resume();
      } catch (...) {
        // Swallow - should be handled in promise
      }
    }
    return;
  }

  if (coro_workers.empty()) {
    // No coroutine workers, run inline (fallback)
    try {
      handle.resume();
    } catch (...) {
      // Swallow - should be handled in promise
    }
    return;
  }

  // Always use the external queue for new coroutine submissions.
  // This avoids deadlock when a coroutine calls .get() on another coroutine
  // it just scheduled - if we pushed to our own local queue, we'd block
  // waiting for work that's in our own queue.
  // The local queue is used for work-stealing and continuations only.
  external_coro_queue->send(handle);

  // Wake one sleeping worker to process the new work
  coro_work_cv.notify_one();
}

// Schedule a regular task on the task pool
void task_scheduler::schedule_task(std::function<void()> task) {
  if (workers.empty()) {
    // No workers, run inline
    task();
    return;
  }

  // Find least busy worker
  std::lock_guard<std::mutex> lock(access_mutex);
  auto now = std::chrono::high_resolution_clock::now();

  worker_info *least_busy = &workers[0];
  for (auto &worker : workers) {
    if (now >= worker.available_at) {
      worker.queue->send(std::move(task));
      return;
    }
    if (least_busy->available_at > worker.available_at)
      least_busy = &worker;
  }

  least_busy->queue->send(std::move(task));
}

// Initialize coroutine workers (called from task_scheduler constructor)
void init_coro_workers(task_scheduler &scheduler) {
  std::size_t num_workers = std::thread::hardware_concurrency();

  // Create the external submission queue
  scheduler.external_coro_queue =
      std::make_shared<ethreads::channel<std::coroutine_handle<>>>();

  scheduler.coro_workers.reserve(num_workers);

  for (std::size_t i = 0; i < num_workers; ++i) {
    auto queue =
        std::make_shared<ethreads::ws_deque<std::coroutine_handle<>>>(1024);

    coro_worker_info info;
    info.coro_queue = queue;
    info.worker_id = i;
    info.scheduler = &scheduler;

    scheduler.coro_workers.push_back(std::move(info));
  }

  // Start worker threads after all infos are created
  for (auto &info : scheduler.coro_workers) {
    info.thread = std::thread(coro_worker, std::ref(info));
  }
}

// Shutdown coroutine workers (called from task_scheduler destructor)
void shutdown_coro_workers(task_scheduler &scheduler) {
  // Note: shutting_down flag is already set by caller

  // Wake all workers that are waiting on the condition variable
  scheduler.coro_work_cv.notify_all();

  // Close the external queue to wake up any blocked workers
  scheduler.external_coro_queue->close();

  // Join all threads
  for (auto &w : scheduler.coro_workers) {
    if (w.thread.joinable()) {
      w.thread.join();
    }
  }

  scheduler.coro_workers.clear();
}
