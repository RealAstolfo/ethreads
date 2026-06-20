#ifndef ETHREADS_TIMER_SERVICE_HPP
#define ETHREADS_TIMER_SERVICE_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <memory>
#include <mutex>
#include <vector>

namespace ethreads {

class cancellation_state;
struct sleep_waiter_state;

// Legacy timer_entry kept raw `atomic<bool>*` + raw `coroutine_handle`,
// which meant a cancelled sleep could leave a dangling pointer in the
// timer heap (awaiter destroyed before its deadline, pointer in heap,
// deadline hits → UAF). The new `waiter` field carries shared ownership
// so the heap entry is self-contained and safe across early cancellation.
// The raw `fired`/`handle` pair is retained for shared_state timed
// awaiters that haven't been migrated yet — those callers keep their
// awaiter alive for the full deadline, so the raw-pointer path stays
// correct in practice.
struct timer_entry {
  std::chrono::steady_clock::time_point deadline;
  std::coroutine_handle<> handle;
  std::shared_ptr<cancellation_state> cancel_state;
  std::atomic<bool> *fired;
  std::shared_ptr<sleep_waiter_state> waiter;

  // Min-heap: earliest deadline has highest priority
  bool operator>(const timer_entry &other) const {
    return deadline > other.deadline;
  }
};

class timer_service {
public:
  timer_service();
  ~timer_service();

  timer_service(const timer_service &) = delete;
  timer_service &operator=(const timer_service &) = delete;

  // Legacy API (raw pointer to awaiter-owned atomic<bool>). Used by
  // shared_state timed awaiters where the awaiter outlives its deadline.
  void add_timer(std::chrono::steady_clock::time_point deadline,
                 std::coroutine_handle<> handle,
                 std::shared_ptr<cancellation_state> cancel_state,
                 std::atomic<bool> *fired);

  // New API (shared ownership of fired/handle). Used by sleep_awaiter
  // so early cancellation can drop the awaiter while the timer heap
  // entry continues to hold the fired state.
  void add_timer(std::chrono::steady_clock::time_point deadline,
                 std::shared_ptr<cancellation_state> cancel_state,
                 std::shared_ptr<sleep_waiter_state> waiter);

  void shutdown();

private:
  void run();

  std::vector<timer_entry> heap_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> running_{true};
  std::thread thread_;
};

timer_service &get_timer_service();

} // namespace ethreads

#endif // ETHREADS_TIMER_SERVICE_HPP
