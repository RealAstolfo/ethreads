#ifndef ETHREADS_CORO_TASK_HPP
#define ETHREADS_CORO_TASK_HPP

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace ethreads {

// Forward declaration for scheduler integration
void schedule_coro_handle(std::coroutine_handle<> handle);

// Shared state for coroutine result communication
template <typename T> struct coro_shared_state {
  std::variant<std::monostate, T, std::exception_ptr> result;
  std::atomic<bool> ready{false};
  std::coroutine_handle<> continuation{nullptr};
  std::mutex mutex;
  std::condition_variable cv;

  void set_value(T value) {
    {
      std::lock_guard lock(mutex);
      result = std::move(value);
      ready.store(true, std::memory_order_release);
    }
    cv.notify_all();
    // Note: continuation is resumed via final_awaiter symmetric transfer,
    // not here. Scheduling here would cause double-resume.
  }

  void set_exception(std::exception_ptr e) {
    {
      std::lock_guard lock(mutex);
      result = e;
      ready.store(true, std::memory_order_release);
    }
    cv.notify_all();
    // Note: continuation is resumed via final_awaiter symmetric transfer,
    // not here. Scheduling here would cause double-resume.
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
  std::coroutine_handle<> continuation{nullptr};
  std::mutex mutex;
  std::condition_variable cv;

  void set_value() {
    {
      std::lock_guard lock(mutex);
      ready.store(true, std::memory_order_release);
    }
    cv.notify_all();
    // Note: continuation is resumed via final_awaiter symmetric transfer,
    // not here. Scheduling here would cause double-resume.
  }

  void set_exception(std::exception_ptr e) {
    {
      std::lock_guard lock(mutex);
      exception = e;
      ready.store(true, std::memory_order_release);
    }
    cv.notify_all();
    // Note: continuation is resumed via final_awaiter symmetric transfer,
    // not here. Scheduling here would cause double-resume.
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
        if (promise.state->continuation) {
          return promise.state->continuation;
        }
        return std::noop_coroutine();
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
        started_(other.started_.load()) {}

  coro_task &operator=(coro_task &&other) noexcept {
    if (this != &other) {
      if (handle_)
        handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
      state_ = std::move(other.state_);
      started_.store(other.started_.load());
    }
    return *this;
  }

  ~coro_task() {
    // Only destroy if the coroutine is done to avoid double-free
    if (handle_) {
      // Wait for coroutine to complete before destroying
      if (state_ && !state_->is_ready()) {
        state_->get(); // Block until done
      }
      if (handle_.done()) {
        handle_.destroy();
      }
    }
  }

  // Awaitable interface for co_await
  bool await_ready() const noexcept {
    return state_ && state_->is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) {
    state_->continuation = awaiting;
    if (!started_.load()) {
      schedule_coro_handle(handle_);
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
        if (promise.state->continuation) {
          return promise.state->continuation;
        }
        return std::noop_coroutine();
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
        started_(other.started_.load()) {}

  coro_task &operator=(coro_task &&other) noexcept {
    if (this != &other) {
      if (handle_)
        handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
      state_ = std::move(other.state_);
      started_.store(other.started_.load());
    }
    return *this;
  }

  ~coro_task() {
    // Only destroy if the coroutine is done to avoid double-free
    if (handle_) {
      // Wait for coroutine to complete before destroying
      if (state_ && !state_->is_ready()) {
        state_->get(); // Block until done
      }
      if (handle_.done()) {
        handle_.destroy();
      }
    }
  }

  bool await_ready() const noexcept {
    return state_ && state_->is_ready();
  }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) {
    state_->continuation = awaiting;
    if (!started_.load()) {
      schedule_coro_handle(handle_);
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
