#include "io_uring_service.hpp"
#include "task_scheduler.hpp"

#include <liburing.h>
#include <cerrno>
#include <cstdio>

namespace ethreads {

static constexpr unsigned RING_ENTRIES = 256;

io_uring_service::io_uring_service() {
  ring_ = new struct io_uring{};

  struct io_uring_params params{};
  params.flags = IORING_SETUP_COOP_TASKRUN;

  int ret = io_uring_queue_init_params(RING_ENTRIES, ring_, &params);
  if (ret < 0) {
    std::fprintf(stderr, "[io_uring] queue_init failed: %d\n", -ret);
    delete ring_;
    ring_ = nullptr;
    return;
  }

  running_.store(true, std::memory_order_release);
  completion_thread_ = std::thread([this] { run(); });
}

io_uring_service::~io_uring_service() { shutdown(); }

io_uring_sqe* io_uring_service::get_sqe() {
  if (!ring_) return nullptr;
  return io_uring_get_sqe(ring_);
}

int io_uring_service::submit() {
  if (!ring_) return -1;
  return io_uring_submit(ring_);
}

void io_uring_service::shutdown() {
  if (!running_.exchange(false, std::memory_order_acq_rel))
    return;  // already shut down

  if (ring_) {
    // Wake the completion thread by submitting a NOP
    auto* sqe = io_uring_get_sqe(ring_);
    if (sqe) {
      io_uring_prep_nop(sqe);
      io_uring_sqe_set_data(sqe, nullptr);
      io_uring_submit(ring_);
    }
  }

  if (completion_thread_.joinable())
    completion_thread_.join();

  if (ring_) {
    io_uring_queue_exit(ring_);
    delete ring_;
    ring_ = nullptr;
  }
}

void io_uring_service::run() {
  while (running_.load(std::memory_order_acquire)) {
    struct io_uring_cqe* cqe = nullptr;
    int ret = io_uring_wait_cqe(ring_, &cqe);
    if (ret < 0) {
      if (ret == -EINTR) continue;
      break;  // fatal error
    }

    auto* comp = static_cast<io_completion*>(io_uring_cqe_get_data(cqe));
    if (comp) {
      comp->result = cqe->res;
      comp->flags = cqe->flags;
      ::schedule_coro_handle(comp->handle);
    }
    // comp == nullptr is our shutdown NOP — just consume it

    io_uring_cqe_seen(ring_, cqe);
  }
}

// Global instance — same pattern as timer_service
static io_uring_service* g_io_uring_service = nullptr;

io_uring_service& get_io_uring_service() { return *g_io_uring_service; }

void init_io_uring_service() {
  g_io_uring_service = new io_uring_service();
}

void shutdown_io_uring_service() {
  if (g_io_uring_service) {
    g_io_uring_service->shutdown();
    delete g_io_uring_service;
    g_io_uring_service = nullptr;
  }
}

} // namespace ethreads
