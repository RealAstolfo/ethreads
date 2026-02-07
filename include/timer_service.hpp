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

struct timer_entry {
  std::chrono::steady_clock::time_point deadline;
  std::coroutine_handle<> handle;
  std::shared_ptr<cancellation_state> cancel_state;
  std::atomic<bool> *fired;

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

  void add_timer(std::chrono::steady_clock::time_point deadline,
                 std::coroutine_handle<> handle,
                 std::shared_ptr<cancellation_state> cancel_state,
                 std::atomic<bool> *fired);

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
