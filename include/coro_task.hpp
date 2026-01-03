#ifndef ETHREADS_CORO_TASK_HPP
#define ETHREADS_CORO_TASK_HPP

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

#include "shared_state/continuation_handoff.hpp"

namespace ethreads {

// Forward declaration for scheduler integration
void schedule_coro_handle(std::coroutine_handle<> handle);

// Shared state for coroutine result communication
template <typename T> struct coro_shared_state {
  std::variant<std::monostate, T, std::exception_ptr> result;
  std::atomic<bool> ready{false};
  std::atomic<bool> final_suspend_reached{false};
  std::atomic<bool> detached_{false};  // For non-blocking destructor
  std::mutex mutex;
  std::condition_variable cv;

  // Lock-free continuation handoff primitive
  continuation_handoff handoff_;

  // Delegate to handoff primitive
  bool try_set_finished() { return handoff_.set_ready(); }
  std::coroutine_handle<> try_set_continuation(std::coroutine_handle<> h) {
    return handoff_.set_continuation(h);
  }
  std::coroutine_handle<> get_continuation() const { return handoff_.get_continuation(); }
  bool is_finished() const { return handoff_.is_ready(); }

  void set_value(T value) {
    std::lock_guard lock(mutex);
    result = std::move(value);
    ready.store(true, std::memory_order_release);
    cv.notify_all();
    // lock released by RAII after notify
  }

  void set_exception(std::exception_ptr e) {
    std::lock_guard lock(mutex);
    result = e;
    ready.store(true, std::memory_order_release);
    cv.notify_all();
    // lock released by RAII after notify
  }

  T get() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return ready.load(std::memory_order_acquire); });
    if (std::holds_alternative<std::exception_ptr>(result)) {
      std::rethrow_exception(std::get<std::exception_ptr>(result));
    }
    return std::get<T>(std::move(result));
  }

  bool is_ready() const { return ready.load(std::memory_order_acquire); }
};

// Specialization for void
template <> struct coro_shared_state<void> {
  std::optional<std::exception_ptr> exception;
  std::atomic<bool> ready{false};
  std::atomic<bool> final_suspend_reached{false};
  std::atomic<bool> detached_{false};  // For non-blocking destructor
  std::mutex mutex;
  std::condition_variable cv;

  // Lock-free continuation handoff primitive
  continuation_handoff handoff_;

  // Delegate to handoff primitive
  bool try_set_finished() { return handoff_.set_ready(); }
  std::coroutine_handle<> try_set_continuation(std::coroutine_handle<> h) {
    return handoff_.set_continuation(h);
  }
  std::coroutine_handle<> get_continuation() const { return handoff_.get_continuation(); }
  bool is_finished() const { return handoff_.is_ready(); }

  void set_value() {
    std::lock_guard lock(mutex);
    ready.store(true, std::memory_order_release);
    cv.notify_all();
    // lock released by RAII after notify
  }

  void set_exception(std::exception_ptr e) {
    std::lock_guard lock(mutex);
    exception = e;
    ready.store(true, std::memory_order_release);
    cv.notify_all();
    // lock released by RAII after notify
  }

  void get() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return ready.load(std::memory_order_acquire); });
    if (exception) {
      std::rethrow_exception(*exception);
    }
  }

  bool is_ready() const { return ready.load(std::memory_order_acquire); }
};

// Primary template for coro_task<T>
template <typename T = void> class coro_task {
public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    std::shared_ptr<coro_shared_state<T>> state =
        std::make_shared<coro_shared_state<T>>();

    coro_task get_return_object() {
      return coro_task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
      bool await_ready() noexcept { return false; }

      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        auto &promise = h.promise();

        // Determine continuation BEFORE signaling completion
        std::coroutine_handle<> continuation = std::noop_coroutine();

        // Single atomic determines who resumes continuation
        if (promise.state->try_set_finished()) {
          // We're responsible for resuming - symmetric transfer
          continuation = promise.state->get_continuation();
        }

        // Signal completion - destructor waits for this before setting detached
        promise.state->final_suspend_reached.store(true,
                                                   std::memory_order_release);

        // CRITICAL: Check detached AFTER setting final_suspend_reached
        // This is the synchronization point with the destructor
        // If detached is set, destructor is waiting and we must self-destruct
        if (promise.state->detached_.load(std::memory_order_acquire)) {
          h.destroy();
          return std::noop_coroutine();
        }

        return continuation;
      }

      void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    void return_value(T value) { state->set_value(std::move(value)); }

    void unhandled_exception() {
      state->set_exception(std::current_exception());
    }
  };

  // Constructors and assignment
  coro_task(const coro_task &) = delete;
  coro_task &operator=(const coro_task &) = delete;

  coro_task(coro_task &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        state_(std::move(other.state_)),
        started_(other.started_.load(std::memory_order_acquire)) {
    // Moving a started task is unsafe - the coroutine may be running
    assert(!started_.load(std::memory_order_relaxed) &&
           "Cannot move a started coro_task - coroutine may be running");
  }

  coro_task &operator=(coro_task &&other) noexcept {
    if (this != &other) {
      // Moving a started task is unsafe - the coroutine may be running
      assert(!other.started_.load(std::memory_order_acquire) &&
             "Cannot move a started coro_task - coroutine may be running");
      if (handle_)
        handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
      state_ = std::move(other.state_);
      started_.store(other.started_.load(std::memory_order_acquire),
                     std::memory_order_release);
    }
    return *this;
  }

  ~coro_task() {
    if (handle_) {
      if (!started_.load(std::memory_order_acquire)) {
        // Task never started - safe to destroy immediately
        handle_.destroy();
      } else if (state_) {
        // Task was started - need to coordinate with final_awaiter
        // Wait for final_suspend to be reached
        while (!state_->final_suspend_reached.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        // Signal coroutine to self-destruct
        // The coroutine checks detached AFTER setting final_suspend_reached
        // so this store will be seen by the coroutine
        state_->detached_.store(true, std::memory_order_release);
        // Coroutine will destroy itself, we just clear our handle
        handle_ = nullptr;
      }
    }
  }

  // Awaitable interface for co_await
  bool await_ready() const noexcept {
    return state_ && state_->is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) {
    // Start task if not already started
    if (!started_.exchange(true, std::memory_order_acq_rel)) {
      schedule_coro_handle(handle_);
    }

    // Single atomic determines who resumes
    auto to_resume = state_->try_set_continuation(awaiting);
    if (to_resume && to_resume != std::noop_coroutine()) {
      // Task already finished - use symmetric transfer to resume immediately
      return awaiting;
    }

    return std::noop_coroutine();
  }

  T await_resume() { return state_->get(); }

  // Blocking get for non-coroutine contexts
  T get() {
    start(); // Start if not already started
    return state_->get();
  }

  // Check if result is ready
  bool is_ready() const { return state_ && state_->is_ready(); }

  // Get the underlying handle (for scheduling)
  handle_type handle() const { return handle_; }

  // Start execution (schedules on worker threads)
  void start() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true) && handle_ &&
        !handle_.done()) {
      schedule_coro_handle(handle_);
    }
  }

  // Check if started
  bool is_started() const { return started_.load(); }

private:
  explicit coro_task(handle_type h)
      : handle_(h), state_(h.promise().state), started_(false) {}

  handle_type handle_;
  std::shared_ptr<coro_shared_state<T>> state_;
  std::atomic<bool> started_;
};

// Specialization for void
template <> class coro_task<void> {
public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    std::shared_ptr<coro_shared_state<void>> state =
        std::make_shared<coro_shared_state<void>>();

    coro_task get_return_object() {
      return coro_task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
      bool await_ready() noexcept { return false; }

      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        auto &promise = h.promise();

        // Determine continuation BEFORE signaling completion
        std::coroutine_handle<> continuation = std::noop_coroutine();

        // Single atomic determines who resumes continuation
        if (promise.state->try_set_finished()) {
          // We're responsible for resuming - symmetric transfer
          continuation = promise.state->get_continuation();
        }

        // Signal completion - destructor waits for this before setting detached
        promise.state->final_suspend_reached.store(true,
                                                   std::memory_order_release);

        // CRITICAL: Check detached AFTER setting final_suspend_reached
        // This is the synchronization point with the destructor
        // If detached is set, destructor is waiting and we must self-destruct
        if (promise.state->detached_.load(std::memory_order_acquire)) {
          h.destroy();
          return std::noop_coroutine();
        }

        return continuation;
      }

      void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    void return_void() { state->set_value(); }

    void unhandled_exception() {
      state->set_exception(std::current_exception());
    }
  };

  coro_task(const coro_task &) = delete;
  coro_task &operator=(const coro_task &) = delete;

  coro_task(coro_task &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        state_(std::move(other.state_)),
        started_(other.started_.load(std::memory_order_acquire)) {
    // Moving a started task is unsafe - the coroutine may be running
    assert(!started_.load(std::memory_order_relaxed) &&
           "Cannot move a started coro_task - coroutine may be running");
  }

  coro_task &operator=(coro_task &&other) noexcept {
    if (this != &other) {
      // Moving a started task is unsafe - the coroutine may be running
      assert(!other.started_.load(std::memory_order_acquire) &&
             "Cannot move a started coro_task - coroutine may be running");
      if (handle_)
        handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
      state_ = std::move(other.state_);
      started_.store(other.started_.load(std::memory_order_acquire),
                     std::memory_order_release);
    }
    return *this;
  }

  ~coro_task() {
    if (handle_) {
      if (!started_.load(std::memory_order_acquire)) {
        // Task never started - safe to destroy immediately
        handle_.destroy();
      } else if (state_) {
        // Task was started - need to coordinate with final_awaiter
        // Wait for final_suspend to be reached
        while (!state_->final_suspend_reached.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        // Signal coroutine to self-destruct
        // The coroutine checks detached AFTER setting final_suspend_reached
        // so this store will be seen by the coroutine
        state_->detached_.store(true, std::memory_order_release);
        // Coroutine will destroy itself, we just clear our handle
        handle_ = nullptr;
      }
    }
  }

  bool await_ready() const noexcept {
    return state_ && state_->is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) {
    // Start task if not already started
    if (!started_.exchange(true, std::memory_order_acq_rel)) {
      schedule_coro_handle(handle_);
    }

    // Single atomic determines who resumes
    auto to_resume = state_->try_set_continuation(awaiting);
    if (to_resume && to_resume != std::noop_coroutine()) {
      // Task already finished - use symmetric transfer to resume immediately
      return awaiting;
    }

    return std::noop_coroutine();
  }

  void await_resume() { state_->get(); }

  void get() {
    start(); // Start if not already started
    state_->get();
  }

  bool is_ready() const { return state_ && state_->is_ready(); }

  handle_type handle() const { return handle_; }

  // Start execution (schedules on worker threads)
  void start() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true) && handle_ &&
        !handle_.done()) {
      schedule_coro_handle(handle_);
    }
  }

  // Check if started
  bool is_started() const { return started_.load(); }

private:
  explicit coro_task(handle_type h)
      : handle_(h), state_(h.promise().state), started_(false) {}

  handle_type handle_;
  std::shared_ptr<coro_shared_state<void>> state_;
  std::atomic<bool> started_;
};

// Type trait for detecting coro_task
template <typename T> struct is_coro_task : std::false_type {};

template <typename T> struct is_coro_task<coro_task<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_coro_task_v = is_coro_task<T>::value;

} // namespace ethreads

#endif
