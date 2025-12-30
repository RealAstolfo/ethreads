#ifndef ETHREADS_YIELD_HPP
#define ETHREADS_YIELD_HPP

#include <coroutine>

namespace ethreads {

// Forward declaration
void schedule_coro_handle(std::coroutine_handle<> handle);

// Awaitable that yields execution back to the scheduler
// Allows other coroutines to run before resuming
struct yield_awaiter {
  bool await_ready() noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    // Re-schedule this coroutine to run later
    schedule_coro_handle(h);
  }

  void await_resume() noexcept {}
};

// Create a yield awaiter - use with co_await
inline yield_awaiter yield() { return {}; }

} // namespace ethreads

#endif
