#include "timer_service.hpp"
#include "task_scheduler.hpp"
#include "cancellation.hpp"
#include "sleep.hpp"

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
    heap_.push_back(timer_entry{deadline, handle,
                                 std::move(cancel_state), fired, nullptr});
    std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
  }
  cv_.notify_one();
}

void timer_service::add_timer(
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<cancellation_state> cancel_state,
    std::shared_ptr<sleep_waiter_state> waiter) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heap_.push_back(timer_entry{deadline, {},
                                 std::move(cancel_state), nullptr,
                                 std::move(waiter)});
    std::push_heap(heap_.begin(), heap_.end(), std::greater<>{});
  }
  cv_.notify_one();
}

namespace {
// Returns (handle_to_resume, fired_atomic_ptr) for an entry, preferring
// the shared sleep_waiter_state if present. Keeps dispatch logic in run()
// and shutdown() identical across legacy/new API paths.
struct fire_view {
  std::coroutine_handle<> handle;
  std::atomic<bool> *fired;
};
fire_view view_of(const timer_entry &e) {
  if (e.waiter)
    return {e.waiter->handle, &e.waiter->fired};
  return {e.handle, e.fired};
}
} // namespace

void timer_service::shutdown() {
  if (!running_.exchange(false, std::memory_order_acq_rel))
    return; // already shut down

  cv_.notify_one();
  if (thread_.joinable())
    thread_.join();

  // Fire all remaining timers so no coroutine hangs
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &entry : heap_) {
    auto fv = view_of(entry);
    if (!fv.fired || !fv.handle)
      continue;
    bool expected = false;
    if (fv.fired->compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
      schedule_coro_handle(fv.handle);
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

      auto fv = view_of(entry);
      if (fv.fired && fv.handle) {
        bool expected = false;
        if (fv.fired->compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
          lock.unlock();
          schedule_coro_handle(fv.handle);
          lock.lock();
        }
      }
    } else {
      // Copy deadline to a local — wait_until releases the mutex, and
      // another thread's add_timer() can trigger vector reallocation,
      // freeing the buffer that `earliest` references.  The libstdc++
      // wait_until reads __atime again after re-acquiring the mutex to
      // check for timeout, so passing a dangling reference is UB.
      auto deadline = earliest.deadline;
      cv_.wait_until(lock, deadline);
    }
  }
}

// Global timer service instance — initialized by task_scheduler
static timer_service *g_timer_service = nullptr;

timer_service &get_timer_service() { return *g_timer_service; }

void init_timer_service() {
  g_timer_service = new timer_service();  // NOASTROGUARD(R3)
}

void shutdown_timer_service() {
  if (g_timer_service) {
    g_timer_service->shutdown();
    delete g_timer_service;  // NOASTROGUARD(R3)
    g_timer_service = nullptr;
  }
}

} // namespace ethreads
