#include "timer_service.hpp"
#include "task_scheduler.hpp"
#include "cancellation.hpp"

#include <algorithm>
#include <thread>

namespace ethreads {

timer_service::timer_service() : thread_([this] { run(); }) {}

timer_service::~timer_service() { shutdown(); }

void timer_service::add_timer(
    std::chrono::steady_clock::time_point deadline,
    std::coroutine_handle<> handle,
    std::shared_ptr<cancellation_state> cancel_state,
    std::atomic<bool> *fired) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heap_.push_back(
        timer_entry{deadline, handle, std::move(cancel_state), fired});
    std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
  }
  cv_.notify_one();
}

void timer_service::shutdown() {
  if (!running_.exchange(false, std::memory_order_acq_rel))
    return; // already shut down

  cv_.notify_one();
  if (thread_.joinable())
    thread_.join();

  // Fire all remaining timers so no coroutine hangs
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &entry : heap_) {
    // Use fired flag to prevent double-resume
    bool expected = false;
    if (entry.fired->compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
      schedule_coro_handle(entry.handle);
    }
  }
  heap_.clear();
}

void timer_service::run() {
  std::unique_lock<std::mutex> lock(mutex_);

  while (running_.load(std::memory_order_acquire)) {
    if (heap_.empty()) {
      cv_.wait(lock, [this] {
        return !heap_.empty() || !running_.load(std::memory_order_acquire);
      });
      continue;
    }

    auto now = std::chrono::steady_clock::now();
    auto &earliest = heap_.front();

    if (earliest.deadline <= now) {
      // Pop the entry
      std::pop_heap(heap_.begin(), heap_.end(), std::greater<>{});
      auto entry = std::move(heap_.back());
      heap_.pop_back();

      // Use fired flag to prevent double-resume with cancellation callback
      bool expected = false;
      if (entry.fired->compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        lock.unlock();
        schedule_coro_handle(entry.handle);
        lock.lock();
      }
    } else {
      cv_.wait_until(lock, earliest.deadline);
    }
  }
}

// Global timer service instance — initialized by task_scheduler
static timer_service *g_timer_service = nullptr;

timer_service &get_timer_service() { return *g_timer_service; }

void init_timer_service() {
  g_timer_service = new timer_service();
}

void shutdown_timer_service() {
  if (g_timer_service) {
    g_timer_service->shutdown();
    delete g_timer_service;
    g_timer_service = nullptr;
  }
}

} // namespace ethreads
