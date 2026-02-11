#ifndef ETHREADS_IO_URING_SERVICE_HPP
#define ETHREADS_IO_URING_SERVICE_HPP

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <thread>

struct io_uring;         // forward-declare liburing's ring type
struct io_uring_sqe;

namespace ethreads {

// Completion context stored as user_data on each SQE.
// The completion thread fills result/flags and resumes the coroutine.
struct io_completion {
  std::coroutine_handle<> handle;
  int result = 0;       // cqe->res (bytes transferred or negative errno)
  uint32_t flags = 0;   // cqe->flags
};

// Singleton service owning a single io_uring ring and a dedicated
// completion-reaping thread.  Modelled after timer_service.
class io_uring_service {
public:
  io_uring_service();
  ~io_uring_service();

  io_uring_service(const io_uring_service&) = delete;
  io_uring_service& operator=(const io_uring_service&) = delete;

  // Get an SQE slot.  Returns nullptr if the ring is full.
  io_uring_sqe* get_sqe();

  // Submit all pending SQEs to the kernel (batched single syscall).
  int submit();

  void shutdown();

private:
  // Blocking completion loop (runs on its own thread).
  void run();

  struct io_uring* ring_ = nullptr;   // heap-allocated to avoid pulling header
  std::atomic<bool> running_{false};
  std::thread completion_thread_;
};

io_uring_service& get_io_uring_service();

// Called from task_scheduler init/shutdown
void init_io_uring_service();
void shutdown_io_uring_service();

} // namespace ethreads

#endif // ETHREADS_IO_URING_SERVICE_HPP
